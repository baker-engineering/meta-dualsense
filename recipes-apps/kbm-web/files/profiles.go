// Profile system: persistent named snapshots of /etc/kbm-mapper.conf.
//
// Each profile is a single .conf file under /etc/kbm-mapper.d/profiles/<id>.conf.
// Metadata (name, description, timestamps) lives in a single header comment line
// of the form:
//
//   # meta {"name":"...","description":"...","created":"...","modified":"..."}
//
// The daemon's config parser ignores '#' lines, so the file is directly
// consumable as a mapper config without any preprocessing. The active profile's
// ID lives in /etc/kbm-mapper.d/active. On activation we copy the profile to
// /etc/kbm-mapper.conf; the daemon's inotify watch picks up the change and
// hot-reloads.

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

const (
	profilesDir   = "/etc/kbm-mapper.d/profiles"
	activeMarker  = "/etc/kbm-mapper.d/active"
	liveConfPath  = "/etc/kbm-mapper.conf"
	metaLinePrefix = "# meta "
)

type Profile struct {
	ID          string    `json:"id"`
	Name        string    `json:"name"`
	Description string    `json:"description"`
	Active      bool      `json:"active"`
	Created     time.Time `json:"created"`
	Modified    time.Time `json:"modified"`
}

type profileMeta struct {
	Name        string    `json:"name"`
	Description string    `json:"description"`
	Created     time.Time `json:"created"`
	Modified    time.Time `json:"modified"`
}

var slugRe = regexp.MustCompile(`[^a-z0-9-]+`)

func slugify(name string) string {
	s := strings.ToLower(strings.TrimSpace(name))
	s = slugRe.ReplaceAllString(s, "-")
	s = strings.Trim(s, "-")
	if s == "" {
		s = "profile"
	}
	if len(s) > 48 {
		s = s[:48]
	}
	return s
}

func profilePath(id string) string {
	return filepath.Join(profilesDir, id+".conf")
}

func readActiveID() string {
	b, err := os.ReadFile(activeMarker)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func writeActiveID(id string) error {
	if err := os.MkdirAll(filepath.Dir(activeMarker), 0o755); err != nil {
		return err
	}
	return os.WriteFile(activeMarker, []byte(id+"\n"), 0o644)
}

// parseProfileMeta scans the first few lines for our metadata comment.
// Returns zero-value meta if absent.
func parseProfileMeta(data []byte) profileMeta {
	var m profileMeta
	for _, line := range strings.SplitN(string(data), "\n", 16) {
		if !strings.HasPrefix(line, metaLinePrefix) {
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			break // first non-comment line ends the metadata header
		}
		_ = json.Unmarshal([]byte(line[len(metaLinePrefix):]), &m)
		return m
	}
	return m
}

// stripProfileMeta returns the .conf bytes with the first '# meta ' line
// removed; used when we rewrite or when the daemon's config-parsing path is
// triggered indirectly through saveConfig.
func stripProfileMeta(data []byte) []byte {
	var out strings.Builder
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, metaLinePrefix) {
			continue
		}
		out.WriteString(line)
		out.WriteByte('\n')
	}
	return []byte(strings.TrimRight(out.String(), "\n") + "\n")
}

func renderProfile(meta profileMeta, body []byte) []byte {
	mb, _ := json.Marshal(meta)
	body = stripProfileMeta(body)
	return []byte(metaLinePrefix + string(mb) + "\n" + string(body))
}

// listProfiles enumerates the profiles directory.
func listProfiles() ([]Profile, error) {
	if err := os.MkdirAll(profilesDir, 0o755); err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(profilesDir)
	if err != nil {
		return nil, err
	}
	active := readActiveID()
	out := make([]Profile, 0, len(entries))
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".conf") {
			continue
		}
		id := strings.TrimSuffix(e.Name(), ".conf")
		data, err := os.ReadFile(profilePath(id))
		if err != nil {
			continue
		}
		m := parseProfileMeta(data)
		if m.Name == "" {
			m.Name = id
		}
		out = append(out, Profile{
			ID:          id,
			Name:        m.Name,
			Description: m.Description,
			Active:      id == active,
			Created:     m.Created,
			Modified:    m.Modified,
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// bootstrapProfiles creates a Default profile from the live /etc/kbm-mapper.conf
// on first request, so users with a pre-profiles install transition cleanly.
func bootstrapProfiles() error {
	ps, err := listProfiles()
	if err != nil {
		return err
	}
	if len(ps) > 0 {
		return nil
	}
	body, err := os.ReadFile(liveConfPath)
	if err != nil {
		if !errors.Is(err, os.ErrNotExist) {
			return err
		}
		body = []byte("# kbm-mapper configuration\n")
	}
	now := time.Now().UTC()
	meta := profileMeta{
		Name:        "Default",
		Description: "Auto-created from /etc/kbm-mapper.conf at first kbm-web start.",
		Created:     now,
		Modified:    now,
	}
	id := "default"
	if err := os.WriteFile(profilePath(id), renderProfile(meta, body), 0o644); err != nil {
		return err
	}
	return writeActiveID(id)
}

// activeProfilePath returns the .conf path of the active profile, or empty
// string if profiles haven't been bootstrapped yet. Used by loadConfig /
// saveConfig in main.go to read/write through the active profile rather than
// the live file directly.
func activeProfilePath() string {
	id := readActiveID()
	if id == "" {
		return ""
	}
	p := profilePath(id)
	if _, err := os.Stat(p); err != nil {
		return ""
	}
	return p
}

// applyActiveToLive copies the active profile's contents (without the metadata
// line) to /etc/kbm-mapper.conf. The atomic tmp+rename triggers the daemon's
// IN_MOVED_TO watch.
func applyActiveToLive() error {
	p := activeProfilePath()
	if p == "" {
		return errors.New("no active profile")
	}
	data, err := os.ReadFile(p)
	if err != nil {
		return err
	}
	tmp := liveConfPath + ".tmp"
	if err := os.WriteFile(tmp, stripProfileMeta(data), 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, liveConfPath)
}

// uniqueProfileID returns an unused profile ID derived from the given name,
// appending -2, -3, ... if the slug is taken.
func uniqueProfileID(name string) string {
	base := slugify(name)
	id := base
	for i := 2; ; i++ {
		if _, err := os.Stat(profilePath(id)); errors.Is(err, os.ErrNotExist) {
			return id
		}
		id = fmt.Sprintf("%s-%d", base, i)
		if i > 999 {
			return id // give up trying to be pretty
		}
	}
}

// === HTTP handlers ===

func handleProfilesList(w http.ResponseWriter, r *http.Request) {
	if err := bootstrapProfiles(); err != nil {
		http.Error(w, "bootstrap: "+err.Error(), 500)
		return
	}
	ps, err := listProfiles()
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(ps)
}

func handleProfileCreate(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Name        string `json:"name"`
		Description string `json:"description"`
		CopyFrom    string `json:"copy_from"` // optional source profile id
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), 400)
		return
	}
	if strings.TrimSpace(req.Name) == "" {
		http.Error(w, "name required", 400)
		return
	}
	if err := bootstrapProfiles(); err != nil {
		http.Error(w, "bootstrap: "+err.Error(), 500)
		return
	}
	var body []byte
	if req.CopyFrom != "" {
		src, err := os.ReadFile(profilePath(req.CopyFrom))
		if err != nil {
			http.Error(w, "copy_from not found", 400)
			return
		}
		body = src
	} else {
		body = []byte("# kbm-mapper configuration\n")
	}
	now := time.Now().UTC()
	meta := profileMeta{
		Name:        req.Name,
		Description: req.Description,
		Created:     now,
		Modified:    now,
	}
	id := uniqueProfileID(req.Name)
	if err := os.WriteFile(profilePath(id), renderProfile(meta, body), 0o644); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(Profile{ID: id, Name: meta.Name, Description: meta.Description, Created: now, Modified: now})
}

func handleProfileUpdate(w http.ResponseWriter, r *http.Request, id string) {
	data, err := os.ReadFile(profilePath(id))
	if err != nil {
		http.Error(w, "not found", 404)
		return
	}
	var req struct {
		Name        string `json:"name"`
		Description string `json:"description"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad json: "+err.Error(), 400)
		return
	}
	meta := parseProfileMeta(data)
	if strings.TrimSpace(req.Name) != "" {
		meta.Name = req.Name
	}
	meta.Description = req.Description
	meta.Modified = time.Now().UTC()
	if err := os.WriteFile(profilePath(id), renderProfile(meta, data), 0o644); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func handleProfileDelete(w http.ResponseWriter, r *http.Request, id string) {
	if readActiveID() == id {
		http.Error(w, "cannot delete active profile; activate another first", 409)
		return
	}
	if err := os.Remove(profilePath(id)); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func handleProfileActivate(w http.ResponseWriter, r *http.Request, id string) {
	if _, err := os.Stat(profilePath(id)); err != nil {
		http.Error(w, "not found", 404)
		return
	}
	if err := writeActiveID(id); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	if err := applyActiveToLive(); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func handleProfileExport(w http.ResponseWriter, r *http.Request, id string) {
	data, err := os.ReadFile(profilePath(id))
	if err != nil {
		http.Error(w, "not found", 404)
		return
	}
	meta := parseProfileMeta(data)
	fname := slugify(meta.Name)
	if fname == "" {
		fname = id
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", fname+".conf"))
	w.Write(data)
}

func handleProfileImport(w http.ResponseWriter, r *http.Request) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		http.Error(w, err.Error(), 400)
		return
	}
	meta := parseProfileMeta(body)
	if strings.TrimSpace(meta.Name) == "" {
		meta.Name = "Imported"
	}
	now := time.Now().UTC()
	if meta.Created.IsZero() {
		meta.Created = now
	}
	meta.Modified = now
	id := uniqueProfileID(meta.Name)
	if err := os.WriteFile(profilePath(id), renderProfile(meta, body), 0o644); err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(Profile{ID: id, Name: meta.Name, Description: meta.Description, Created: meta.Created, Modified: meta.Modified})
}

// registerProfileRoutes wires the profile endpoints into mux. Paths:
//   GET  /api/profiles
//   POST /api/profiles
//   POST /api/profiles/import
//   GET  /api/profiles/{id}/export
//   POST /api/profiles/{id}/activate
//   PUT  /api/profiles/{id}
//   DELETE /api/profiles/{id}
func registerProfileRoutes(mux *http.ServeMux) {
	mux.HandleFunc("/api/profiles", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			handleProfilesList(w, r)
		case http.MethodPost:
			handleProfileCreate(w, r)
		default:
			http.Error(w, "method not allowed", 405)
		}
	})
	mux.HandleFunc("/api/profiles/import", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", 405)
			return
		}
		handleProfileImport(w, r)
	})
	mux.HandleFunc("/api/profiles/", func(w http.ResponseWriter, r *http.Request) {
		rest := strings.TrimPrefix(r.URL.Path, "/api/profiles/")
		parts := strings.Split(rest, "/")
		if len(parts) == 0 || parts[0] == "" {
			http.Error(w, "missing id", 400)
			return
		}
		id := parts[0]
		if len(parts) == 1 {
			switch r.Method {
			case http.MethodPut:
				handleProfileUpdate(w, r, id)
			case http.MethodDelete:
				handleProfileDelete(w, r, id)
			default:
				http.Error(w, "method not allowed", 405)
			}
			return
		}
		switch parts[1] {
		case "activate":
			if r.Method != http.MethodPost {
				http.Error(w, "method not allowed", 405)
				return
			}
			handleProfileActivate(w, r, id)
		case "export":
			if r.Method != http.MethodGet {
				http.Error(w, "method not allowed", 405)
				return
			}
			handleProfileExport(w, r, id)
		default:
			http.Error(w, "unknown subresource", 404)
		}
	})
}
