//! The one place design tokens live. Every colour and font stack in the app comes from
//! here; the stylesheet below is the only consumer, so there is no second copy to drift.
//!
//! The palette is the same as the Qt build (`legacy-qt/src/ui/theme/generated/tokens.json`),
//! and the CJK fallback chain is explicit because the design's Funnel Sans / Inter / Geist
//! have no CJK glyphs (AGENTS.md rule 8).

pub const ACCENT: &str = "#6C5CE7";
pub const ACCENT2: &str = "#A78BFA";
pub const INK: &str = "#262040";
pub const TRACK: &str = "#EDE8DA";
pub const DANGER: &str = "#DE5B4C";

pub const STYLESHEET: &str = r#"
:root {
  --surface: #F7F4EC;
  --card: #FFFFFF;
  --ink: #262040;
  --ink-2: #6F6985;
  --line: #E8E2D3;
  --accent: #6C5CE7;
  --accent-soft: #EFECFF;
  --accent-2: #A78BFA;
  --accent-3: #E49AB0;
  --sidebar: #2A2450;
  --sidebar-text: #B7B1D6;
  --success: #3BA776;
  --danger: #DE5B4C;
  --warning: #E0A23F;
  --track: #EDE8DA;
}

* { box-sizing: border-box; }
html, body, #main { height: 100%; margin: 0; }
body {
  font-family: "MiSans", "Inter", "Noto Sans SC", "Microsoft YaHei", "PingFang SC", sans-serif;
  font-size: 13px;
  color: var(--ink);
  background: var(--surface);
  overflow: hidden;
  -webkit-font-smoothing: antialiased;
  user-select: none;
}
button { font: inherit; color: inherit; border: 0; background: none; cursor: pointer; padding: 0; }
input, select, textarea { font: inherit; }
::-webkit-scrollbar { width: 8px; height: 8px; }
::-webkit-scrollbar-thumb { background: #D9D3C4; border-radius: 4px; }
::-webkit-scrollbar-thumb:hover { background: #C4BDAC; }
::-webkit-scrollbar-track { background: transparent; }

.app { display: flex; flex-direction: column; height: 100vh; min-height: 0; }

/* ---------------------------------------------------------------- title bar */
.titlebar {
  height: 46px; flex: 0 0 46px;
  display: flex; align-items: center; gap: 10px;
  padding: 0 8px 0 16px;
  background: var(--card);
  border-bottom: 1px solid var(--line);
}
.titlebar .drag { display: flex; align-items: center; gap: 10px; flex: 1; height: 100%; }
.brand-img { width: 22px; height: 22px; flex: 0 0 22px; object-fit: contain; }
.brand-name { font-weight: 600; font-size: 13px; letter-spacing: .2px; }
.app-tagline { font-size: 12px; color: var(--ink-2); }
.win-controls { display: flex; gap: 2px; }
.winbtn {
  width: 38px; height: 30px; border-radius: 8px;
  display: flex; align-items: center; justify-content: center;
  color: var(--ink-2);
}
.winbtn:hover { background: var(--surface); color: var(--ink); }
.winbtn.close:hover { background: var(--danger); color: #fff; }

/* ---------------------------------------------------------------- shell */
.body { flex: 1; display: flex; min-height: 0; }
.sidebar {
  width: 216px; flex: 0 0 216px;
  background: var(--sidebar); color: var(--sidebar-text);
  display: flex; flex-direction: column;
  padding: 18px 14px 14px;
}
.side-brand { display: flex; align-items: center; gap: 10px; padding: 0 10px 14px; }
.side-name { color: #fff; font-weight: 500; font-size: 19px; line-height: 1.2; }
.side-cap { font-size: 10px; letter-spacing: 1.4px; color: var(--sidebar-text); margin-top: 2px; }
.nav { display: flex; flex-direction: column; gap: 3px; }
.nav-item {
  display: flex; align-items: center; gap: 9px;
  padding: 8px 12px; border-radius: 8px;
  color: var(--sidebar-text); font-size: 13px; text-align: left;
  transition: background .15s ease, color .15s ease;
}
.nav-item .ico { width: 17px; display: flex; justify-content: center; opacity: .9; }
.nav-item:hover { background: rgba(255, 255, 255, .08); color: #fff; }
.nav-item.active { background: var(--accent); color: #fff; font-weight: 500; }
.nav-item.active .ico { color: #fff; opacity: 1; }
.promo {
  margin-top: auto;
  background: linear-gradient(150deg, rgba(108, 92, 231, .42), rgba(228, 154, 176, .28));
  border: 1px solid rgba(255, 255, 255, .10);
  border-radius: 12px; padding: 14px; color: #fff;
}
.promo h4 { margin: 0 0 5px; font-size: 12px; font-weight: 600; }
.promo p { margin: 0; font-size: 11px; line-height: 1.55; color: rgba(255, 255, 255, .72); }

.content { flex: 1; min-width: 0; display: flex; flex-direction: column; padding: 0 24px 14px; gap: 12px; }
.page { flex: 1; min-height: 0; overflow: auto; padding-top: 18px; display: flex; flex-direction: column; gap: 12px; }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 14px; box-shadow: 0 6px 18px rgba(38, 32, 64, .04); }

/* ---------------------------------------------------------------- status bar */
.statusbar {
  height: 36px; flex: 0 0 36px;
  display: flex; align-items: center; gap: 14px;
  padding: 0 14px; overflow: hidden;
  background: var(--card); border: 1px solid var(--line); border-radius: 12px;
}
.chip { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--ink-2); white-space: nowrap; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: #C9C3B4; flex: 0 0 8px; }
.dot.running { background: var(--success); box-shadow: 0 0 0 3px rgba(59, 167, 118, .14); }
.dot.starting, .dot.switching { background: var(--warning); animation: pulse 1.4s ease-in-out infinite; }
.dot.crashed { background: var(--danger); }
.dot.stopped, .dot.idle { background: #C9C3B4; }
@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: .35; } }
.spacer { flex: 1; }
.statusbar .version { font-size: 11px; color: var(--ink-2); }

/* ---------------------------------------------------------------- home */
.announce { display: flex; align-items: center; gap: 10px; padding: 8px 14px; font-size: 12px; color: var(--ink-2); }
.announce .link { margin-left: auto; color: var(--accent); }
.suite { padding: 14px 18px; display: flex; flex-direction: column; min-height: 160px; }
.section-head { display: flex; align-items: center; gap: 10px; padding-bottom: 8px; }
.accent-bar { width: 4px; border-radius: 2px; background: var(--accent); height: 18px; flex: 0 0 4px; }
.accent-bar.pink { background: var(--accent-3); }
.accent-bar.violet { background: var(--accent-2); }
.section-title { font-weight: 600; font-size: 14px; }
.hint { font-size: 12px; color: var(--ink-2); }
.suite-body { flex: 1; min-height: 0; overflow: auto; display: flex; flex-direction: column; }
.rowline { display: flex; align-items: center; gap: 12px; padding: 8px 8px; border-radius: 10px; }
.rowline:hover { background: #FBFAF5; }
.svc-id { min-width: 0; flex: 1; }
.svc-name { font-weight: 500; font-size: 13px; display: flex; align-items: center; gap: 8px; }
.svc-meta { font-size: 11px; color: var(--ink-2); margin-top: 2px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.badge { font-size: 11px; border-radius: 6px; padding: 1px 6px; font-weight: 500; }
.badge.default { background: var(--accent-soft); color: var(--accent); }
.badge.secondary { background: var(--surface); color: var(--ink-2); }
.port { font-family: ui-monospace, Consolas, monospace; font-size: 12px; color: var(--ink-2); }
.empty-hint { padding: 28px 8px; color: var(--ink-2); font-size: 12px; text-align: center; }

.switch { width: 38px; height: 21px; flex: 0 0 38px; border-radius: 11px; background: #D8D2C4; position: relative; transition: background .18s ease; }
.switch.on { background: var(--accent); }
.switch.locked { opacity: .55; cursor: default; }
.switch .knob { position: absolute; top: 2px; left: 2px; width: 17px; height: 17px; border-radius: 50%; background: #fff; transition: left .18s ease; box-shadow: 0 1px 2px rgba(0, 0, 0, .22); }
.switch.on .knob { left: 19px; }
.icon-btn { width: 28px; height: 28px; flex: 0 0 28px; border-radius: 8px; display: flex; align-items: center; justify-content: center; color: var(--ink-2); }
.icon-btn:hover { background: var(--surface); color: var(--accent); }
.icon-btn:disabled { opacity: .35; cursor: default; }

.bottom { display: flex; gap: 14px; height: 218px; flex: 0 0 218px; }
.menu-card { width: 168px; flex: 0 0 168px; padding: 10px; display: flex; flex-direction: column; gap: 4px; }
.menu-btn { height: 34px; border-radius: 9px; padding: 0 10px; display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--ink-2); }
.menu-btn:hover { background: var(--surface); }
.menu-btn.active { background: var(--accent-soft); color: var(--accent); font-weight: 500; }
.pane { flex: 1; min-width: 0; padding: 14px 16px; display: flex; gap: 20px; overflow: hidden; }
.gauge { width: 108px; flex: 0 0 108px; text-align: center; }
.gauge svg { display: block; margin: 0 auto; }
.gauge .g-label { font-size: 12px; color: var(--ink-2); margin-top: 4px; }
.gauge .g-value { font-size: 15px; font-weight: 600; margin-top: 1px; }
.divider { width: 1px; background: var(--line); align-self: stretch; flex: 0 0 1px; }
.hw { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 9px; overflow: hidden; }
.hw .model { font-size: 12px; font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.hw .sub { font-size: 11px; color: var(--ink-2); }
.meter-row { display: flex; align-items: center; gap: 10px; font-size: 11px; color: var(--ink-2); }
.meter-row .name { width: 68px; flex: 0 0 68px; }
.meter { height: 8px; border-radius: 4px; background: var(--track); overflow: hidden; flex: 1; }
.meter > i { display: block; height: 100%; border-radius: 4px; background: linear-gradient(90deg, var(--accent), var(--accent-2)); transition: width .6s ease; }
.meter.violet > i { background: linear-gradient(90deg, var(--accent-2), var(--accent-3)); }
.meter-row .pct { width: 46px; flex: 0 0 46px; text-align: right; color: var(--ink); font-variant-numeric: tabular-nums; }

.resource { flex: 1; min-width: 0; display: flex; gap: 18px; }
.resource .col { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 9px; }
.resource .col-title { font-size: 11px; color: var(--ink-2); }
.kv { display: flex; gap: 10px; font-size: 12px; align-items: baseline; min-width: 0; }
.kv .k { width: 60px; flex: 0 0 60px; color: var(--ink-2); }
.kv .v { color: var(--ink); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.logview {
  flex: 1; min-width: 0; background: #241F45; color: #D8D3F0;
  border-radius: 10px; padding: 10px 12px;
  font-family: ui-monospace, Consolas, monospace; font-size: 11px; line-height: 1.65;
  overflow: auto; white-space: pre-wrap; user-select: text;
}
.pane-head { display: flex; align-items: center; gap: 8px; padding-bottom: 6px; }

/* ---------------------------------------------------------------- tables */
.toolbar { display: flex; align-items: center; gap: 10px; padding: 14px 18px 12px; }
.toolbar .title { font-size: 15px; font-weight: 600; }
.toolbar .sub { font-size: 12px; color: var(--ink-2); margin-top: 2px; }
.search {
  height: 30px; width: 220px; border: 1px solid var(--line); border-radius: 8px;
  padding: 0 10px 0 30px; background: #FBFAF5; font-size: 12px; outline: none;
}
.search:focus { border-color: var(--accent); background: #fff; }
.search-wrap { position: relative; display: flex; align-items: center; }
.search-wrap .ico { position: absolute; left: 9px; color: var(--ink-2); display: flex; }
.table { width: 100%; border-collapse: collapse; font-size: 12px; }
.table th {
  text-align: left; font-weight: 500; color: var(--ink-2);
  padding: 8px 14px; background: #FCFBF7; border-top: 1px solid var(--line); border-bottom: 1px solid var(--line);
}
.table td { padding: 10px 14px; border-bottom: 1px solid #F2EEE3; vertical-align: middle; }
.table tr:last-child td { border-bottom: 0; }
.table tbody tr:hover { background: #FBFAF5; }
.cell-strong { font-weight: 500; }
.cell-sub { font-size: 11px; color: var(--ink-2); margin-top: 2px; }
.mono { font-family: ui-monospace, Consolas, monospace; }
.pill { display: inline-flex; align-items: center; gap: 5px; font-size: 11px; border-radius: 20px; padding: 2px 8px; background: var(--surface); color: var(--ink-2); }
.pill.ok { background: rgba(59, 167, 118, .12); color: var(--success); }
.pill.bad { background: rgba(222, 91, 76, .10); color: var(--danger); }
.pill.warn { background: rgba(224, 162, 63, .14); color: #B07C24; }

/* ---------------------------------------------------------------- buttons / forms */
.btn {
  height: 32px; padding: 0 14px; border-radius: 8px;
  background: var(--accent); color: #fff; font-size: 12px; font-weight: 500;
  display: inline-flex; align-items: center; gap: 6px; justify-content: center;
  transition: background .15s ease;
}
.btn:hover { background: #5B4BD6; }
.btn:disabled { background: #CFC9E6; cursor: default; }
.btn.danger { background: var(--danger); }
.btn.danger:hover { background: #C74C3E; }
.soft-btn {
  height: 30px; padding: 0 12px; border-radius: 8px;
  border: 1px solid var(--line); background: var(--card); font-size: 12px;
  display: inline-flex; align-items: center; gap: 6px; justify-content: center;
}
.soft-btn:hover { border-color: var(--accent); color: var(--accent); }
.soft-btn:disabled { opacity: .5; cursor: default; }
.link-btn { color: var(--accent); font-size: 12px; }
.link-btn:hover { text-decoration: underline; }
.link-btn.danger { color: var(--danger); }

.overlay {
  position: fixed; inset: 0; z-index: 50;
  background: rgba(38, 32, 64, .38);
  display: flex; align-items: center; justify-content: center;
}
.dialog {
  width: 520px; max-width: calc(100vw - 48px); max-height: 86vh; overflow: auto;
  background: var(--card); border-radius: 16px;
  box-shadow: 0 24px 60px rgba(38, 32, 64, .28);
  padding: 22px 24px 18px;
}
.dialog h3 { margin: 0 0 6px; font-size: 16px; }
.dialog .msg { font-size: 13px; line-height: 1.7; color: var(--ink-2); white-space: pre-wrap; }
.field { display: flex; align-items: center; gap: 12px; margin: 10px 0; }
.field > label { width: 86px; flex: 0 0 86px; font-size: 12px; color: var(--ink-2); text-align: right; }
.field .grow { flex: 1; min-width: 0; display: flex; align-items: center; gap: 8px; }
.input {
  height: 32px; border: 1px solid var(--line); border-radius: 8px;
  padding: 0 10px; background: #fff; flex: 1; min-width: 0; font-size: 12px; outline: none;
}
.input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px rgba(108, 92, 231, .12); }
.input.mono { font-family: ui-monospace, Consolas, monospace; }
.dialog-actions { display: flex; justify-content: flex-end; gap: 10px; margin-top: 18px; }
.error { color: var(--danger); font-size: 12px; margin-top: 8px; white-space: pre-wrap; }
.field-hint { font-size: 11px; color: var(--ink-2); }
.radio-row { display: flex; gap: 16px; align-items: center; }
.radio { display: inline-flex; align-items: center; gap: 6px; font-size: 12px; color: var(--ink-2); }
.radio.on { color: var(--accent); font-weight: 500; }

.toast {
  position: fixed; right: 24px; bottom: 62px; z-index: 60;
  max-width: 440px; background: #2A2450; color: #fff;
  border-radius: 10px; padding: 10px 14px; font-size: 12px; line-height: 1.6;
  box-shadow: 0 12px 30px rgba(38, 32, 64, .3);
  white-space: pre-wrap;
}
.toast.error { background: var(--danger); }

.card-pad { padding: 16px 20px; }
.settings-block { display: flex; flex-direction: column; gap: 2px; }
.setting-row { display: flex; align-items: center; gap: 14px; padding: 12px 0; border-bottom: 1px solid #F2EEE3; }
.setting-row:last-child { border-bottom: 0; }
.setting-row .text { flex: 1; min-width: 0; }
.setting-row .name { font-size: 13px; font-weight: 500; }
.setting-row .desc { font-size: 11px; color: var(--ink-2); margin-top: 3px; line-height: 1.5; }
.update-line { display: flex; align-items: center; gap: 10px; font-size: 12px; color: var(--ink-2); }
"#;

pub fn state_key(state: ddp_core::instance::RunState) -> &'static str {
    use ddp_core::instance::RunState;
    match state {
        RunState::Running => "running",
        RunState::Starting => "starting",
        RunState::Switching => "switching",
        RunState::Crashed => "crashed",
        RunState::Idle => "idle",
        RunState::Stopped => "stopped",
    }
}

