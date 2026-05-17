// kbm-web: small HTTP UI to configure /etc/kbm-mapper.conf, restart the
// dualsense-gadget / kbm-mapper services, and stream the live HID report
// state from /run/kbm-mapper/state.bin to a browser.
//
// Single-process, stdlib-only, runs as root so it can write /etc/ and call
// systemctl. Designed for LAN use; no auth.
package main

import (
	"embed"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

//go:embed ui
var uiFS embed.FS

const (
	confPath  = "/etc/kbm-mapper.conf"
	statePath = "/run/kbm-mapper/state.bin"
	udcGlob   = "/sys/class/udc/*"
)

type Tuning struct {
	WindowMs     float64 `json:"window_ms"`
	CurveExp     float64 `json:"curve_exp"`
	AntiDeadzone float64 `json:"anti_deadzone"`
	OuterSat     float64 `json:"outer_sat"`
	SensCountsMs float64 `json:"sens_counts_ms"`
	DebtDrain    float64 `json:"debt_drain"`

	// Recoil compensation: while RecoilAction is held the daemon adds a
	// constant bias (RecoilX, RecoilY) to the right stick. Action names
	// match the daemon's action vocabulary ("r2", "l2", etc.). Empty
	// RecoilAction disables compensation.
	RecoilAction string  `json:"recoil_action"`
	RecoilX      float64 `json:"recoil_x"`
	RecoilY      float64 `json:"recoil_y"`

	// Sensitivity scaling tied to action state. While AdsAction (typically
	// "l2") is held, right-stick output is scaled by AdsSensScale. While
	// RecoilAction is held, it is scaled by FireSensScale. Both default
	// to 1.0 (no scaling).
	AdsAction     string  `json:"ads_action"`
	AdsSensScale  float64 `json:"ads_sens_scale"`
	FireSensScale float64 `json:"fire_sens_scale"`
}

type Binding struct {
	Source string `json:"source"` // "key" or "mouse"
	Code   string `json:"code"`
	Action string `json:"action"`
}

// Burst describes burst-on-hold for one DualSense action. While the source
// input is held the mapper oscillates the reported action at Hz with the
// given duty fraction (0..1). Hz <= 0 means disabled.
type Burst struct {
	Action string  `json:"action"`
	Hz     float64 `json:"hz"`
	Duty   float64 `json:"duty"`
	// Jitter in [0, 1]: per-cycle perturbation of cycle length and duty
	// by +/- (jitter * 100)%. Makes a held burst non-periodic so it does
	// not present a machine-clean cadence to anti-cheat heuristics. 0
	// disables (clockwork burst).
	Jitter float64 `json:"jitter"`
}

type Config struct {
	Tuning   Tuning    `json:"tuning"`
	Bindings []Binding `json:"bindings"`
	Bursts   []Burst   `json:"bursts"`
	// Hotkey is the mode-toggle chord; ordered list of KEY_* names (without
	// the KEY_ prefix), e.g. ["LEFTCTRL","ESC"]. All keys must be held
	// simultaneously for >=1 s to fire. Empty list disables the hotkey.
	Hotkey []string `json:"hotkey"`
}

type Status struct {
	Hostname     string   `json:"hostname"`
	GadgetUDC    string   `json:"gadget_udc"`
	UDCState     string   `json:"udc_state"`
	UDCSpeed     string   `json:"udc_speed"`
	HidgPresent  bool     `json:"hidg_present"`
	ReportDesc   int64    `json:"report_desc_size"`
	InputDevices []string `json:"input_devices"`
	Mode         string   `json:"mode"` // "emulation" | "passthrough" | "off"
	GadgetActive string   `json:"gadget_service"`
	MapperActive string   `json:"mapper_service"`
	Version      string   `json:"version"`
}

var (
	defaultTuning = Tuning{
		WindowMs: 6, CurveExp: 2.0, AntiDeadzone: 0.10,
		OuterSat: 0.97, SensCountsMs: 8.0, DebtDrain: 0.05,
		RecoilAction: "", RecoilX: 0, RecoilY: 0,
		AdsAction: "", AdsSensScale: 1.0, FireSensScale: 1.0,
	}
	buildVersion = "dev"
)

func main() {
	addr := envOr("KBM_WEB_ADDR", ":80")

	mux := http.NewServeMux()
	uiSub, err := fs.Sub(uiFS, "ui")
	if err != nil {
		log.Fatalf("ui embed: %v", err)
	}
	mux.Handle("/", http.FileServer(http.FS(uiSub)))
	mux.HandleFunc("/api/status", handleStatus)
	mux.HandleFunc("/api/config", handleConfig)
	mux.HandleFunc("/api/restart", handleRestart)
	mux.HandleFunc("/api/state", handleStateSSE)
	mux.HandleFunc("/api/mode", handleMode)
	mux.HandleFunc("/api/capture", handleCaptureSSE)
	registerProfileRoutes(mux)
	_ = bootstrapProfiles()

	srv := &http.Server{
		Addr:              addr,
		Handler:           logMW(mux),
		ReadHeaderTimeout: 5 * time.Second,
	}
	log.Printf("kbm-web %s listening on %s", buildVersion, addr)
	log.Fatal(srv.ListenAndServe())
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func logMW(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/api/state") {
			log.Printf("%s %s", r.Method, r.URL.Path)
		}
		h.ServeHTTP(w, r)
	})
}

func readTrim(p string) string {
	b, err := os.ReadFile(p)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func systemctlIsActive(unit string) string {
	out, _ := exec.Command("systemctl", "is-active", unit).Output()
	return strings.TrimSpace(string(out))
}

func firstUDC() string {
	matches, _ := filepath.Glob(udcGlob)
	for _, m := range matches {
		return filepath.Base(m)
	}
	return ""
}

func currentMode() string {
	if systemctlIsActive("dualsense-ffsd") == "active" {
		return "emulation"
	}
	if systemctlIsActive("kbm-passthrough") == "active" {
		return "passthrough"
	}
	return "off"
}

func handleStatus(w http.ResponseWriter, r *http.Request) {
	udc := firstUDC()
	mode := currentMode()
	s := Status{
		Hostname:     readTrim("/proc/sys/kernel/hostname"),
		GadgetUDC:    readTrim("/sys/kernel/config/usb_gadget/dualsense/UDC"),
		Version:      buildVersion,
		Mode:         mode,
		GadgetActive: systemctlIsActive("dualsense-ffs"),
		MapperActive: systemctlIsActive("dualsense-ffsd"),
	}
	if mode == "passthrough" {
		s.GadgetUDC = readTrim("/sys/kernel/config/usb_gadget/kbm/UDC")
	}
	if udc != "" {
		s.UDCState = readTrim("/sys/class/udc/" + udc + "/state")
		s.UDCSpeed = readTrim("/sys/class/udc/" + udc + "/current_speed")
	}
	if fi, err := os.Stat("/dev/hidg0"); err == nil && fi.Mode()&os.ModeCharDevice != 0 {
		s.HidgPresent = true
	}
	if fi, err := os.Stat("/etc/dualsense/report_desc.bin"); err == nil {
		s.ReportDesc = fi.Size()
	}
	matches, _ := filepath.Glob("/sys/class/input/event*/device/name")
	seen := map[string]bool{}
	for _, m := range matches {
		if name := readTrim(m); name != "" && !seen[name] {
			s.InputDevices = append(s.InputDevices, name)
			seen[name] = true
		}
	}
	sort.Strings(s.InputDevices)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(s)
}

func handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		c, err := loadConfig()
		if err != nil {
			http.Error(w, err.Error(), 500)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(c)
	case http.MethodPost:
		var c Config
		if err := json.NewDecoder(r.Body).Decode(&c); err != nil {
			http.Error(w, "bad json: "+err.Error(), 400)
			return
		}
		if err := saveConfig(&c); err != nil {
			http.Error(w, "save: "+err.Error(), 500)
			return
		}
		// dualsense-ffsd hot-reloads /etc/kbm-mapper.conf via inotify on
		// IN_MOVED_TO (we just renamed the tmp file into place), so no
		// service restart is needed — eliminates the brief USB downtime
		// that would otherwise hit on every tuning slider change.
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", 405)
	}
}

func loadConfig() (*Config, error) {
	c := &Config{Tuning: defaultTuning}
	data, err := os.ReadFile(confPath)
	if err != nil {
		if os.IsNotExist(err) {
			return c, nil
		}
		return nil, err
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}
		k := strings.TrimSpace(line[:eq])
		v := strings.TrimSpace(line[eq+1:])
		switch k {
		case "window_ms":
			c.Tuning.WindowMs = parseF(v)
		case "curve_exp":
			c.Tuning.CurveExp = parseF(v)
		case "anti_deadzone":
			c.Tuning.AntiDeadzone = parseF(v)
		case "outer_sat":
			c.Tuning.OuterSat = parseF(v)
		case "sens_counts_ms":
			c.Tuning.SensCountsMs = parseF(v)
		case "debt_drain":
			c.Tuning.DebtDrain = parseF(v)
		case "recoil.action":
			c.Tuning.RecoilAction = strings.ToLower(strings.TrimSpace(v))
		case "recoil.x":
			c.Tuning.RecoilX = parseF(v)
		case "recoil.y":
			c.Tuning.RecoilY = parseF(v)
		case "ads.action":
			c.Tuning.AdsAction = strings.ToLower(strings.TrimSpace(v))
		case "ads.sens_scale":
			c.Tuning.AdsSensScale = parseF(v)
		case "fire.sens_scale":
			c.Tuning.FireSensScale = parseF(v)
		default:
			if rest, ok := strings.CutPrefix(k, "key."); ok {
				c.Bindings = append(c.Bindings, Binding{Source: "key", Code: rest, Action: v})
			} else if rest, ok := strings.CutPrefix(k, "mouse."); ok {
				c.Bindings = append(c.Bindings, Binding{Source: "mouse", Code: rest, Action: v})
			} else if rest, ok := strings.CutPrefix(k, "burst."); ok {
				// "burst.<action>.<field>", field in {hz, duty, jitter}
				dot := strings.LastIndexByte(rest, '.')
				if dot < 0 {
					continue
				}
				act := rest[:dot]
				field := rest[dot+1:]
				b := findBurst(&c.Bursts, act)
				switch field {
				case "hz":
					b.Hz = parseF(v)
				case "duty":
					b.Duty = parseF(v)
				case "jitter":
					b.Jitter = parseF(v)
				}
			} else if k == "hotkey.mode_toggle" {
				c.Hotkey = nil
				for _, part := range splitChord(v) {
					part = strings.TrimSpace(part)
					if part != "" {
						c.Hotkey = append(c.Hotkey, strings.ToUpper(part))
					}
				}
			}
		}
	}
	sort.SliceStable(c.Bindings, func(i, j int) bool {
		if c.Bindings[i].Source != c.Bindings[j].Source {
			return c.Bindings[i].Source < c.Bindings[j].Source
		}
		return c.Bindings[i].Code < c.Bindings[j].Code
	})
	sort.SliceStable(c.Bursts, func(i, j int) bool {
		return c.Bursts[i].Action < c.Bursts[j].Action
	})
	if len(c.Hotkey) == 0 {
		// Mirror the daemon's default so the UI shows the right chord even
		// when the config file predates the feature.
		c.Hotkey = []string{"LEFTCTRL", "ESC"}
	}
	return c, nil
}

// splitChord splits "A+B+C" or "A,B,C" into ["A","B","C"]; either separator
// is accepted because the daemon parser is lenient about both.
func splitChord(v string) []string {
	v = strings.ReplaceAll(v, ",", "+")
	return strings.Split(v, "+")
}

// joinChord renders the hotkey as it appears in the .conf file.
func joinChord(parts []string) string {
	return strings.Join(parts, "+")
}

func findBurst(list *[]Burst, action string) *Burst {
	for i := range *list {
		if (*list)[i].Action == action {
			return &(*list)[i]
		}
	}
	*list = append(*list, Burst{Action: action})
	return &(*list)[len(*list)-1]
}

func parseF(s string) float64 {
	v, _ := strconv.ParseFloat(s, 64)
	return v
}

func saveConfig(c *Config) error {
	var b strings.Builder
	b.WriteString("# Managed by kbm-web. Hand edits are preserved on read but get rewritten on save.\n\n")
	fmt.Fprintf(&b, "window_ms=%g\n", c.Tuning.WindowMs)
	fmt.Fprintf(&b, "curve_exp=%g\n", c.Tuning.CurveExp)
	fmt.Fprintf(&b, "anti_deadzone=%g\n", c.Tuning.AntiDeadzone)
	fmt.Fprintf(&b, "outer_sat=%g\n", c.Tuning.OuterSat)
	fmt.Fprintf(&b, "sens_counts_ms=%g\n", c.Tuning.SensCountsMs)
	fmt.Fprintf(&b, "debt_drain=%g\n", c.Tuning.DebtDrain)
	// Recoil + sensitivity-scale block. Each line is emitted independently
	// when it has a meaningful (non-default) value so an action selection
	// is not silently dropped because its associated magnitudes are still
	// at defaults.
	if c.Tuning.RecoilAction != "" {
		fmt.Fprintf(&b, "recoil.action=%s\n", c.Tuning.RecoilAction)
	}
	if c.Tuning.RecoilX != 0 {
		fmt.Fprintf(&b, "recoil.x=%g\n", c.Tuning.RecoilX)
	}
	if c.Tuning.RecoilY != 0 {
		fmt.Fprintf(&b, "recoil.y=%g\n", c.Tuning.RecoilY)
	}
	if c.Tuning.AdsAction != "" {
		fmt.Fprintf(&b, "ads.action=%s\n", c.Tuning.AdsAction)
	}
	if c.Tuning.AdsSensScale > 0 && c.Tuning.AdsSensScale != 1.0 {
		fmt.Fprintf(&b, "ads.sens_scale=%g\n", c.Tuning.AdsSensScale)
	}
	if c.Tuning.FireSensScale > 0 && c.Tuning.FireSensScale != 1.0 {
		fmt.Fprintf(&b, "fire.sens_scale=%g\n", c.Tuning.FireSensScale)
	}
	b.WriteString("\n")
	for _, bd := range c.Bindings {
		if bd.Source != "key" && bd.Source != "mouse" {
			continue
		}
		fmt.Fprintf(&b, "%s.%s=%s\n", bd.Source, strings.ToUpper(bd.Code), bd.Action)
	}
	for _, br := range c.Bursts {
		if br.Action == "" || br.Hz <= 0 {
			continue
		}
		fmt.Fprintf(&b, "burst.%s.hz=%g\n", br.Action, br.Hz)
		if br.Duty > 0 {
			fmt.Fprintf(&b, "burst.%s.duty=%g\n", br.Action, br.Duty)
		}
		if br.Jitter > 0 {
			fmt.Fprintf(&b, "burst.%s.jitter=%g\n", br.Action, br.Jitter)
		}
	}
	if len(c.Hotkey) > 0 {
		fmt.Fprintf(&b, "hotkey.mode_toggle=%s\n", joinChord(c.Hotkey))
	}
	body := []byte(b.String())
	tmp := confPath + ".tmp"
	if err := os.WriteFile(tmp, body, 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, confPath); err != nil {
		return err
	}
	// Mirror into the active profile so user tweaks persist beyond a profile
	// switch. Metadata header is preserved; only body is updated.
	if ap := activeProfilePath(); ap != "" {
		prev, _ := os.ReadFile(ap)
		meta := parseProfileMeta(prev)
		meta.Modified = time.Now().UTC()
		_ = os.WriteFile(ap, renderProfile(meta, body), 0o644)
	}
	return nil
}

func handleRestart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", 405)
		return
	}
	unit := r.URL.Query().Get("unit")
	switch unit {
	case "dualsense-ffsd", "dualsense-ffs", "kbm-passthrough", "kbm-passthrough-setup":
	default:
		http.Error(w, "unknown unit", 400)
		return
	}
	go func() {
		_ = exec.Command("systemctl", "reset-failed", unit).Run()
		_ = exec.Command("systemctl", "restart", unit).Run()
	}()
	w.WriteHeader(http.StatusNoContent)
}

func handleMode(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"mode": currentMode()})
	case http.MethodPost:
		var req struct {
			Mode string `json:"mode"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "bad json: "+err.Error(), 400)
			return
		}
		switch req.Mode {
		case "emulation":
			go switchTo([]string{"kbm-passthrough", "kbm-passthrough-setup"},
				[]string{"dualsense-ffsd"})
		case "passthrough":
			go switchTo([]string{"dualsense-ffsd", "dualsense-ffs"},
				[]string{"kbm-passthrough"})
		case "off":
			go switchTo([]string{"dualsense-ffsd", "dualsense-ffs",
				"kbm-passthrough", "kbm-passthrough-setup"}, nil)
		default:
			http.Error(w, "mode must be emulation|passthrough|off", 400)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	default:
		http.Error(w, "method not allowed", 405)
	}
}

// switchTo stops any services in `stop`, then starts the ones in `start`.
// Systemd Conflicts= on the unit files would also stop the opposite side,
// but doing it explicitly here keeps the transition deterministic.
func switchTo(stop, start []string) {
	for _, u := range stop {
		_ = exec.Command("systemctl", "stop", u).Run()
	}
	for _, u := range start {
		_ = exec.Command("systemctl", "reset-failed", u).Run()
		_ = exec.Command("systemctl", "start", u).Run()
	}
}

func handleStateSSE(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", 500)
		return
	}
	tick := time.NewTicker(50 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-tick.C:
			buf, err := os.ReadFile(statePath)
			if err != nil || len(buf) < 11 {
				fmt.Fprintf(w, "event: stale\ndata: \n\n")
			} else {
				fmt.Fprintf(w, "data: %s\n\n", hex.EncodeToString(buf))
			}
			flusher.Flush()
		}
	}
}
