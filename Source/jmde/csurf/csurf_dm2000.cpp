#include "csurf.h"
#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#else
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include "dm2000_compat.h"

// Plugin version - shown on the startup splash. Keep in sync with res.rc and the README badge.
#define DM2000_CSURF_VERSION "v0.8"

// DM2000 fader taper calibrated via DM2000 Editor after running the console's built-in
// fader calibration utility (MIDI-OX capture 2026-06-15; doc/fader-calibration-2026-06-15.txt).
// 14-bit wire value = (MSB<<7)|LSB. Physical maximum = 16383 at the printed +10 mark.
// Taper is linear from -30 to +10 dB (1600 wire units per 5 dB); compressed below -30.
static const double g_taper_db[]  = { -150.0, -50.0, -40.0, -30.0, -20.0, -15.0, -10.0,  -5.0,   0.0,  5.0, 10.0 };
static const int    g_taper_val[] = {      0,  2768,  3568,  5168,  6768,  8368,  9968, 11568, 13168, 14768, 16383 };
#define TAPER_N (sizeof(g_taper_val) / sizeof(g_taper_val[0]))

static double int14ToVol(unsigned char msb, unsigned char lsb)
{
    int val = lsb | (msb << 7);
    if (val <= g_taper_val[0]) return 0.0; // bottom = -inf
    if (val >= g_taper_val[TAPER_N - 1]) return DB2VAL(g_taper_db[TAPER_N - 1]);

    unsigned int i = 1;
    while (i < TAPER_N - 1 && val > g_taper_val[i]) i++;
    double db = g_taper_db[i - 1] + (g_taper_db[i] - g_taper_db[i - 1]) *
        (double)(val - g_taper_val[i - 1]) / (double)(g_taper_val[i] - g_taper_val[i - 1]);
    return DB2VAL(db);
}

static int volToInt14(double vol)
{
    double db = VAL2DB(vol);
    if (db <= g_taper_db[0]) return g_taper_val[0];
    if (db >= g_taper_db[TAPER_N - 1]) return g_taper_val[TAPER_N - 1];

    unsigned int i = 1;
    while (i < TAPER_N - 1 && db > g_taper_db[i]) i++;
    double v = g_taper_val[i - 1] + (g_taper_val[i] - g_taper_val[i - 1]) *
        (db - g_taper_db[i - 1]) / (g_taper_db[i] - g_taper_db[i - 1]);
    return (int)(v + 0.5);
}

static unsigned char peakToMeter(double pk)
{
    // the DM2000's red OVER segment IS level 0x0C -- it ignores the 0x0E "clip"
    // code from the HUI docs (hardware-verified: 0x0E never lit the red LED).
    // Send 0x0C only strictly above 0 dBFS so red matches REAPER's indication.
    if (pk > 1.0) return 0x0C;

    // map -60..0 dB onto segments 0x00..0x0B, keeping 0x0C exclusive to clip
    int l = (int)((VAL2DB(pk) + 60.0) * (12.0 / 60.0) + 0.5);
    if (l < 0) l = 0;
    else if (l > 0x0B) l = 0x0B;
    return (unsigned char)l;
}

static unsigned char panToChar(double pan)
{
    pan = (pan + 1.0) * 63.5;

    if (pan < 0.0) pan = 0.0;
    else if (pan > 127.0) pan = 127.0;

    return (unsigned char)(pan + 0.5);
}

static void GetIniPath(char *buf, int bufsz); // defined below; used in constructor

class CSurf_DM2000 : public IReaperControlSurface
{
    int m_midi_in_devs[4];
    int m_midi_out_devs[4];
    midi_Output *m_midiouts[4];
    midi_Input *m_midiins[4];
    midi_Output *m_midiout8;         // DM2000 native SysEx output (port 8); reserved for scene recall

    int m_zone[4];                   // last switch-matrix zone select (B0 0F zz) per port
    unsigned char m_fader_msb[4][8]; // pending fader value MSB per port/channel
    char m_fader_touch[32];
    DWORD m_fader_touchtime[32];     // last fader-touch press time per channel (double-tap detect)
    char m_snap_pending[32];         // double-tap armed on the 2nd press; the 0 dB snap fires on release
    DWORD m_auto_label_time[24];     // when the AUTO key was pressed per channel (momentary mode label on strip)
    DWORD m_winflash_time;           // when EFFECTS DISPLAY toggled FX-window auto-float (brief LCD flash)
    MediaTrack *m_acc_tr[16];        // shared knob accumulator (surround + SEL): target track per slot
    int m_acc_fx[16];                // target fx index per slot
    int m_acc_param[16];             // target param index per slot
    double m_acc_val[16];            // accumulated normalized value per slot - written absolutely, never read back
    int m_acc_next;                  // round-robin eviction pointer
    DWORD m_fxdisp_throttle;         // last live FX-display refresh from a surround knob (~30 Hz throttle)
    char m_splash_done;              // startup "REAPER online" splash shown once (clears desk Off-Line text)
    DWORD m_splash_start;            // first-Run timestamp, splash timing base
    unsigned char m_splash_render;   // last splash on/off state sent (de-dupe)
    bool m_sel_exclusive;            // [select] exclusive: single SEL selects only that track (Pro Tools-style)
    unsigned char m_sel_held[32];    // SELECT buttons currently held (for hold-to-multi-select)
    int m_auto_button;               // [channel] auto_button: 0=unity (reset fader to 0 dB), 1=automation (cycle track mode), 2=monitor (cycle rec-monitor)
    bool m_double_touch;             // [channel] double_touch: double-tap a fader to snap it to 0 dB
    bool m_show_ins;                 // [display] insert_icon: light INS (sw6) when a track has FX
    bool m_show_auto;                // [display] auto_indicator: light AUTO (sw4) when a track is in touch/write/latch
    char m_ins_state[32];            // cached INS icon state per channel (-1 = unknown)
    char m_auto_state[32];           // cached AUTO indicator state per channel (-1 = unknown)
    bool m_blink_phase;              // shared blink phase for blinking indicators (e.g. AUTO=Write)
    DWORD m_blink_last;              // last blink-phase flip
    int m_vol_lastpos[32];
    int m_pan_lastpos[32];
    unsigned char m_enc_ring_last[24]; // last send-level ring sent per channel (live-poll change detect)
    unsigned char m_enc_blink_last[24]; // last V-pot ring-blink (sw5) state per channel (send-mode flag)
    unsigned int m_pan_lasttouch[32];
    unsigned char m_meter_lastlvl[32];
    unsigned char m_meter_hist[32][2][3]; // per-channel/side level history: peak hold over 3 polls
    int m_meter_histpos;
    DWORD m_meter_lastrun;
    int m_bank_offset;
    int m_wheel_mode;                // 0=jog (edit cursor), 1=scrub, 2=shuttle (coarse)
    int m_arrow_mode;                // ENTER cycles cursor arrows: 0=scroll, 1=zoom, 2=horizontal arrange zoom (decoupled from faders)
    int m_auto_mode;                 // 0=trim/bypass, 1=read, 2=touch, 3=write, 4=latch, 5=latch preview
    char m_ini_path[MAX_PATH];       // user-configured path to dm2000_keys.ini (empty = default)
    int m_held_arrow;                // -1=none, 0-3=CSurf_OnArrow direction being held
    DWORD m_arrow_held_since;        // timestamp of the press that started the hold
    DWORD m_arrow_last_repeat;       // timestamp of the last auto-repeat fire
    int m_held_transport;            // -1=none, 1=REW, 2=FF
    DWORD m_transport_held_since;
    DWORD m_transport_last_repeat;
    unsigned char m_tc_lastbuf[8];   // encoded HUI counter bytes for change detection
    // Locate section: configurable action IDs loaded from [locate] in dm2000_keys.ini.
    // RTZ/END: 0 = use CSurf_GoStart()/CSurf_GoEnd() API; non-zero = dispatch that action ID.
    // All others: 0 = no action; non-zero = dispatch that action ID.
    int m_la_rtz, m_la_end, m_la_online, m_la_loop, m_la_qpunch; // zone 0x0F (sw0/1/2/3/4)
    int m_la_audition, m_la_pre, m_la_in, m_la_out, m_la_post;    // zone 0x10 (sw0/1/2/3/4)
    int m_automix_suspend;                           // [automix] suspend: action for the ENABLE button (PT SUSPEND)
    int m_ow_fader, m_ow_on, m_ow_pan, m_ow_aux, m_ow_auxon, m_ow_eq; // [automix] OVERWRITE param-arm buttons -> actions
    int m_la_lm[8];                                  // LM1-LM8 locate memory buttons
    int m_udk[16];                                   // UDK 1-16 action IDs from [udk]; 0 = no action
    int m_tc_interval;                               // LED counter refresh period (ms); [counter] refresh_ms
    DWORD m_tc_lastrun;                              // last counter refresh (decoupled from the meter poll)
    int m_tc_lastmode;                              // last time-display mode driven to the zone 0x16 LEDs (-1 = unknown)
    char m_surround_plugin[64];                      // [surround] target FX name
    int m_surround_param[5];                         // [surround] base param indices (object 1): X, Y, Z, spread, gain
    int m_surround_stride;                           // [surround] params per object (0 = no object switching)
    int m_surround_objects;                          // [surround] max object count (0 = unlimited)
    int m_surround_obj;                              // currently selected object (0-based)
    DWORD m_direct_time;                             // Direct button press time (double-click detect)
    bool m_direct_pending;                           // Direct single-click awaiting confirm
    int m_fx_loglast;                                // last console-logged fx*1000+param (de-spam)
    bool m_console_log;                              // [debug] console = 1: log param/object changes
    int m_routing_led_state;                         // cached routing-LED display (0=none, 1-8=lit button)
    int m_fx_slot;                                   // current FX slot on the selected track (FX editor)
    int m_fx_page;                                   // current parameter page (4 knobs per page)
    DWORD m_fx_page_last;                            // last page-arrow event (debounce; arrows fire 0x4C twice)
    int m_enc_send;                                  // channel-encoder mode: -1 = pan, 0-4 = send slot (AUX A-E)
    bool m_enc_sends;                                // [encoder] sends: AUX SELECT switches encoders to send-level mode
    bool m_flip;                                     // FADER MODE / FLIP: in send mode, faders ride the send, encoders ride volume
    bool m_flip_enabled;                             // [fader] flip: enable the FADER MODE button as FLIP
    int m_send_fader_last[24];                       // FLIP: last send-level fader value sent per channel (change cache)
    char m_scribble_name[24][8];                     // cached track name per surface channel (for override restore)
    int m_scribble_peek;                             // held scribble override: 0 = none, 1 = track number, 2 = fader dB
    bool m_peek_number;                              // [display] peek_number: hold ENC ASSIGN1 to show track numbers
    bool m_peek_db;                                  // [display] peek_db: hold ENC ASSIGN2 to show fader dB
    bool m_peek_latch;                               // [display] peek_latch: ENC ASSIGN1/2 toggle (press locks on/off) instead of momentary
    bool m_touch_db;                                 // [display] touch_db: a touched fader's strip shows its dB until release
    bool m_drive_select_assign;                      // [display] select_assign: drive the SELECT ASSIGN readout (Pan/SndA-E)
    bool m_drive_cursor_mode;                        // [display] cursor_mode: drive the CURSOR MODE readout (NAV/ZOOM)
    bool m_window_on_knob;                           // [fx] window_on_knob: float the FX window when a param knob moves
    char m_sa_pan[8];                                // [labels] pan: SELECT ASSIGN text in pan mode
    char m_sa_send[5][8];                            // [labels] aux1-5: SELECT ASSIGN text per send slot A-E
    int m_general_in, m_general_out;                 // GENERAL port device indices (-1 = none/disabled)
    midi_Input *m_general_midiin;                    // GENERAL port input (scene-recall PC receive)
    midi_Output *m_general_midiout;                  // GENERAL port output (scene-recall PC send)
    bool m_scene_send;                               // [scene] send: #SCENE marker -> drive console (recall while playing, display while stopped)
    bool m_scene_receive;                            // [scene] receive: DM2000 scene PC -> marker jump
    char m_scene_prefix[32];                         // [scene] marker_prefix: scene-marker tag (default "#SCENE")
    double m_scene_lastpos;                          // last play position, for marker-cross detection (playback)
    double m_scene_cursorpos;                        // last edit-cursor position seen while stopped (settle detect)
    DWORD m_scene_cursortime;                        // when the stopped cursor last moved (settle timer)
    int m_scene_applied;                             // last scene number driven to the console (de-dupe re-fires)
    int m_scene_echo[8];                             // ring of scenes we just recalled (suppress the console's PC echoes; -1 = empty)
    DWORD m_scene_echo_t[8];                          // matching send timestamps (echo-suppression window)
    int m_scene_echo_pos;                            // next write slot in the echo ring

public:
  CSurf_DM2000(int indev1, int outdev1, int indev2, int outdev2, int indev3, int outdev3, int indev4, int outdev4, int genIn, int genOut, const char *iniPath, int *errStats)
  {
    m_general_in = genIn;
    m_general_out = genOut;
    m_general_midiin = NULL;
    m_general_midiout = NULL;
    m_scene_send = false;
    m_scene_receive = true;  // documented default ON; matches the ini default even when no ini path resolves (macOS)
    strcpy_s(m_scene_prefix, sizeof(m_scene_prefix), "#SCENE");
    m_scene_lastpos = -1.0;
    m_scene_cursorpos = -1.0;
    m_scene_cursortime = 0;
    m_scene_applied = -1;
    for (int e = 0; e < 8; ++e) { m_scene_echo[e] = -1; m_scene_echo_t[e] = 0; }
    m_scene_echo_pos = 0;
    m_midi_in_devs[0] = indev1;
    m_midi_out_devs[0] = outdev1;
    m_midi_in_devs[1] = indev2;
    m_midi_out_devs[1] = outdev2;
    m_midi_in_devs[2] = indev3;
    m_midi_out_devs[2] = outdev3;
    m_midi_in_devs[3] = indev4;
    m_midi_out_devs[3] = outdev4;

    m_bank_offset = 0;
    m_zone[0] = m_zone[1] = m_zone[2] = m_zone[3] = -1;
    memset(m_fader_msb, 0, sizeof(m_fader_msb));
    memset(m_fader_touch, 0, sizeof(m_fader_touch));
    m_sel_exclusive = true;                 // Pro Tools-style by default; [select] exclusive = 0 to disable
    memset(m_sel_held, 0, sizeof(m_sel_held));
    m_auto_button = 1;                      // [channel] auto_button = automation by default
    m_double_touch = true;                  // [channel] double_touch: snap fader to 0 dB on double-tap (on by default)
    memset(m_fader_touchtime, 0, sizeof(m_fader_touchtime));
    memset(m_snap_pending, 0, sizeof(m_snap_pending));
    memset(m_auto_label_time, 0, sizeof(m_auto_label_time));
    m_winflash_time = 0;
    memset(m_acc_tr, 0, sizeof(m_acc_tr)); memset(m_acc_fx, 0, sizeof(m_acc_fx)); memset(m_acc_param, 0, sizeof(m_acc_param));
    memset(m_acc_val, 0, sizeof(m_acc_val)); m_acc_next = 0; m_fxdisp_throttle = 0;
    m_splash_done = 0; m_splash_start = 0; m_splash_render = 0;
    m_show_ins = m_show_auto = true;        // [display] indicators on by default
    memset(m_ins_state, 0xff, sizeof(m_ins_state));   // 0xff = unknown -> first poll sends
    memset(m_auto_state, 0xff, sizeof(m_auto_state));
    m_blink_phase = true;
    m_blink_last = 0;
    memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
    memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
    memset(m_enc_ring_last, 0xff, sizeof(m_enc_ring_last));
    memset(m_enc_blink_last, 0xff, sizeof(m_enc_blink_last));
    memset(m_send_fader_last, 0xff, sizeof(m_send_fader_last));
    memset(m_pan_lasttouch, 0, sizeof(m_pan_lasttouch));
    memset(m_meter_lastlvl, 0xff, sizeof(m_meter_lastlvl));
    memset(m_meter_hist, 0, sizeof(m_meter_hist));
    m_meter_histpos = 0;
    m_meter_lastrun = 0;
    m_wheel_mode = 0;
    m_arrow_mode = 0;
    m_auto_mode  = 1;
    sprintf_s(m_ini_path, sizeof(m_ini_path), "%s", iniPath ? iniPath : "");
    m_held_arrow = -1;
    m_arrow_held_since = 0;
    m_arrow_last_repeat = 0;
    m_held_transport = -1;
    m_transport_held_since = 0;
    m_transport_last_repeat = 0;
    memset(m_tc_lastbuf, 0xFF, sizeof(m_tc_lastbuf));

    // locate section: all buttons default to no-op; configure via [locate] in dm2000_keys.ini.
    // RTZ/END: 0 = CSurf_GoStart/GoEnd API (fundamental transport, always works).
    // All other buttons: 0 = no action until ini is configured.
    m_la_rtz     = 0;
    m_la_end     = 0;
    m_la_online  = 0;
    m_la_loop    = 0;
    m_la_qpunch  = 0;
    m_la_audition = 0;
    m_la_pre     = 0;
    m_la_in      = 0;
    m_la_out     = 0;
    m_la_post    = 0;
    m_automix_suspend = 0;
    m_ow_fader = m_ow_on = m_ow_pan = m_ow_aux = m_ow_auxon = m_ow_eq = 0;
    for (int i = 0; i < 8; i++)
        m_la_lm[i] = 0;

    // UDK 1-16: all no-op until configured via [udk] in dm2000_keys.ini
    for (int i = 0; i < 16; i++)
        m_udk[i] = 0;
    m_tc_interval = 33;   // ~30 Hz default; smooth SMPTE frame display
    m_tc_lastrun  = 0;
    m_tc_lastmode = -1;
    // surround: compiled default is "none" (disabled), like [locate]/[udk]. The working
    // ReaSurroundPan map ships in dm2000_keys.ini.example; with no [surround] section
    // configured, the MCS PANNER knobs/joystick do nothing.
    m_surround_plugin[0] = '\0';
    for (int i = 0; i < 5; i++) m_surround_param[i] = -1;
    m_surround_stride = 0;
    m_surround_objects = 0;
    m_surround_obj = 0;
    m_direct_time = 0;
    m_direct_pending = false;
    m_fx_loglast = -1;
    m_console_log = false;
    m_routing_led_state = -1;
    m_fx_slot = 0;
    m_fx_page = 0;
    m_fx_page_last = 0;
    m_enc_send = -1;                         // start in pan mode
    m_enc_sends = true;                      // [encoder] sends on by default
    m_flip = false;                          // FLIP off until the FADER MODE button toggles it
    m_flip_enabled = true;                   // [fader] flip on by default
    memset(m_scribble_name, 0, sizeof(m_scribble_name));
    m_scribble_peek = 0;
    m_peek_number = m_peek_db = true;        // [display] peek modifiers on by default
    m_peek_latch = false;                    // [display] peek_latch: momentary by default
    m_touch_db = true;                       // [display] touch_db: show dB while riding a fader (on by default)
    m_drive_select_assign = true;            // [display] select_assign: drive SELECT ASSIGN by default
    m_drive_cursor_mode = true;              // [display] cursor_mode: drive CURSOR MODE by default
    m_window_on_knob = true;                 // [fx] window_on_knob: float FX window on knob move by default
    strcpy_s(m_sa_pan, sizeof(m_sa_pan), "Pan ");
    for (int i = 0; i < 5; ++i)
    { m_sa_send[i][0]='S'; m_sa_send[i][1]='n'; m_sa_send[i][2]='d'; m_sa_send[i][3]=(char)('A'+i); m_sa_send[i][4]=0; }

    // load [locate] overrides from ini
    char _la_ini[MAX_PATH];
    if (m_ini_path[0])
        sprintf_s(_la_ini, sizeof(_la_ini), "%s", m_ini_path);
    else
        GetIniPath(_la_ini, sizeof(_la_ini));
    if (_la_ini[0])
    {
        m_la_rtz      = GetPrivateProfileInt("locate", "rtz",      m_la_rtz,      _la_ini);
        m_la_end      = GetPrivateProfileInt("locate", "end",      m_la_end,      _la_ini);
        m_la_online   = GetPrivateProfileInt("locate", "online",   m_la_online,   _la_ini);
        m_la_loop     = GetPrivateProfileInt("locate", "loop",     m_la_loop,     _la_ini);
        m_la_qpunch   = GetPrivateProfileInt("locate", "qpunch",   m_la_qpunch,   _la_ini);
        m_la_audition = GetPrivateProfileInt("locate", "audition", m_la_audition, _la_ini);
        m_la_pre      = GetPrivateProfileInt("locate", "pre",      m_la_pre,      _la_ini);
        m_la_in       = GetPrivateProfileInt("locate", "in",       m_la_in,       _la_ini);
        m_la_out      = GetPrivateProfileInt("locate", "out",      m_la_out,      _la_ini);
        m_la_post     = GetPrivateProfileInt("locate", "post",     m_la_post,     _la_ini);
        // [automix] OVERWRITE param-arm buttons + ENABLE (SUSPEND): no native REAPER role -> action IDs (0 = none)
        m_automix_suspend = GetPrivateProfileInt("automix", "suspend",  m_automix_suspend, _la_ini);
        m_ow_fader        = GetPrivateProfileInt("automix", "ow_fader", m_ow_fader,        _la_ini);
        m_ow_on           = GetPrivateProfileInt("automix", "ow_on",    m_ow_on,           _la_ini);
        m_ow_pan          = GetPrivateProfileInt("automix", "ow_pan",   m_ow_pan,          _la_ini);
        m_ow_aux          = GetPrivateProfileInt("automix", "ow_aux",   m_ow_aux,          _la_ini);
        m_ow_auxon        = GetPrivateProfileInt("automix", "ow_auxon", m_ow_auxon,        _la_ini);
        m_ow_eq           = GetPrivateProfileInt("automix", "ow_eq",    m_ow_eq,           _la_ini);
        for (int i = 0; i < 8; i++)
        {
            char key[8];
            sprintf_s(key, sizeof(key), "lm%d", i + 1);
            m_la_lm[i] = GetPrivateProfileInt("locate", key, m_la_lm[i], _la_ini);
        }
        // [udk] key1..key16 -> Main_OnCommand action IDs (0 = no action)
        for (int i = 0; i < 16; i++)
        {
            char key[8];
            sprintf_s(key, sizeof(key), "key%d", i + 1);
            m_udk[i] = GetPrivateProfileInt("udk", key, m_udk[i], _la_ini);
        }
        // [counter] refresh_ms: LED timecode refresh period (clamped 20..1000 ms)
        m_tc_interval = GetPrivateProfileInt("counter", "refresh_ms", m_tc_interval, _la_ini);
        if (m_tc_interval < 20) m_tc_interval = 20;
        else if (m_tc_interval > 1000) m_tc_interval = 1000;

        // [surround] target plugin + ReaSurroundPan param indices for the MCS PANNER knobs
        char pbuf[64];
        GetPrivateProfileString("surround", "plugin", m_surround_plugin, pbuf, sizeof(pbuf), _la_ini);
        strcpy_s(m_surround_plugin, sizeof(m_surround_plugin), pbuf);
        // knobs map to neighbouring controls X, Y, Z, spread, gain (in that order)
        m_surround_param[0] = GetPrivateProfileInt("surround", "param_x",      m_surround_param[0], _la_ini);
        m_surround_param[1] = GetPrivateProfileInt("surround", "param_y",      m_surround_param[1], _la_ini);
        m_surround_param[2] = GetPrivateProfileInt("surround", "param_z",      m_surround_param[2], _la_ini);
        m_surround_param[3] = GetPrivateProfileInt("surround", "param_spread", m_surround_param[3], _la_ini);
        m_surround_param[4] = GetPrivateProfileInt("surround", "param_gain",   m_surround_param[4], _la_ini);
        m_surround_stride   = GetPrivateProfileInt("surround", "stride",  m_surround_stride,  _la_ini);
        m_surround_objects  = GetPrivateProfileInt("surround", "objects", m_surround_objects, _la_ini);

        // [select] exclusive = 1 (default): SEL selects only that track, hold to multi-select
        m_sel_exclusive = GetPrivateProfileInt("select", "exclusive", 1, _la_ini) != 0;

        // [channel] auto_button: what the per-channel AUTO key does (default = automation)
        char abuf[32] = "";
        GetPrivateProfileString("channel", "auto_button", "automation", abuf, sizeof(abuf), _la_ini);
        if (!_stricmp(abuf, "unity"))        m_auto_button = 0;
        else if (!_stricmp(abuf, "monitor")) m_auto_button = 2;
        else                                 m_auto_button = 1; // "automation" (default)
        m_double_touch = GetPrivateProfileInt("channel", "double_touch", 1, _la_ini) != 0;

        // [display] per-channel indicators (default on)
        m_show_ins  = GetPrivateProfileInt("display", "insert_icon",    1, _la_ini) != 0;
        m_show_auto = GetPrivateProfileInt("display", "auto_indicator", 1, _la_ini) != 0;
        // [display] momentary scribble peeks: hold ENC ASSIGN 1/2 to overlay track number / fader dB (default on)
        m_peek_number = GetPrivateProfileInt("display", "peek_number", 1, _la_ini) != 0;
        m_peek_db     = GetPrivateProfileInt("display", "peek_db",     1, _la_ini) != 0;
        m_peek_latch  = GetPrivateProfileInt("display", "peek_latch",  0, _la_ini) != 0;
        m_touch_db    = GetPrivateProfileInt("display", "touch_db",    1, _la_ini) != 0;
        m_drive_select_assign = GetPrivateProfileInt("display", "select_assign", 1, _la_ini) != 0;
        m_drive_cursor_mode   = GetPrivateProfileInt("display", "cursor_mode",   1, _la_ini) != 0;
        m_window_on_knob      = GetPrivateProfileInt("fx",      "window_on_knob", 1, _la_ini) != 0;
        // [labels] override the 4-char SELECT ASSIGN strings (pan, aux1-5); default = built-in
        { char def[8]; strcpy_s(def, sizeof(def), m_sa_pan);
          GetPrivateProfileString("labels", "pan", def, m_sa_pan, sizeof(m_sa_pan), _la_ini); }
        { static const char *auxkey[5] = { "aux1","aux2","aux3","aux4","aux5" };
          for (int i = 0; i < 5; ++i)
          { char def[8]; strcpy_s(def, sizeof(def), m_sa_send[i]);
            GetPrivateProfileString("labels", auxkey[i], def, m_sa_send[i], sizeof(m_sa_send[i]), _la_ini); } }
        // [encoder] sends = 1 (default): AUX SELECT switches channel encoders to send-level mode
        m_enc_sends = GetPrivateProfileInt("encoder", "sends", 1, _la_ini) != 0;
        // [fader] flip: FADER MODE button swaps faders <-> send encoders (send mode only)
        m_flip_enabled = GetPrivateProfileInt("fader", "flip", 1, _la_ini) != 0;
        m_console_log = GetPrivateProfileInt("debug", "console", 0, _la_ini) != 0;

        // [scene] scene recall via the GENERAL port (only active if a GENERAL port is set).
        // receive defaults ON (harmless - just moves the cursor, and inert without a
        // GENERAL port); send defaults OFF (it actively drives the console's scenes).
        m_scene_send    = GetPrivateProfileInt("scene", "send",    0, _la_ini) != 0;
        m_scene_receive = GetPrivateProfileInt("scene", "receive", 1, _la_ini) != 0;
        char sbuf[32];
        GetPrivateProfileString("scene", "marker_prefix", m_scene_prefix, sbuf, sizeof(sbuf), _la_ini);
        if (sbuf[0]) strcpy_s(m_scene_prefix, sizeof(m_scene_prefix), sbuf);
    }

    for (int i = 0; i < 4; ++i)
    {
        m_midiins[i] = m_midi_in_devs[i] >= 0 ? CreateMIDIInput(m_midi_in_devs[i]) : NULL;
        m_midiouts[i] = m_midi_out_devs[i] >= 0 ? CreateThreadedMIDIOutput(CreateMIDIOutput(m_midi_out_devs[i], false, NULL)) : NULL;

        if (errStats)
        {
            if (m_midi_in_devs[i] >= 0 && !m_midiins[i]) *errStats |= 1;
            if (m_midi_out_devs[i] >= 0 && !m_midiouts[i]) *errStats |= 2;
        }

        if (m_midiins[i])
            m_midiins[i]->start();
    }

    m_midiout8 = NULL;

    // GENERAL port (arbitrary USB port set as the console's GENERAL Rx/Tx) - opened
    // separately from the 4 HUI ports, only when configured, for scene recall.
    // Deliberately NOT reported via errStats like the HUI ports: scene recall is an
    // optional add-on, so a busy/absent GENERAL port silently disables just that
    // feature (all call sites are NULL-guarded) instead of flagging the whole surface.
    if (m_general_in >= 0)
    {
        m_general_midiin = CreateMIDIInput(m_general_in);
        if (m_general_midiin) m_general_midiin->start();
    }
    if (m_general_out >= 0)
        m_general_midiout = CreateThreadedMIDIOutput(CreateMIDIOutput(m_general_out, false, NULL));

    SendCounter(true);  // blank counter display and sync m_tc_lastbuf to known state
    SetEncModeLEDs();    // light ENC PAN at startup so the encoder mode is visible (default = pan)
    RefreshSelectAssign(); // show "Pan" in the SELECT ASSIGN readout at startup
    SendCursorMode(0);     // show NAVIGATION in the CURSOR MODE readout at startup
    SendGlobalLED(0x0D, 5, false); // clear stale SCRUB / SHUTTLE wheel-mode LEDs (wheel starts in jog)
    SendGlobalLED(0x0D, 6, false);
  }

  ~CSurf_DM2000()
  {
      CloseNoReset();
  }

  const char *GetTypeString() { return "DM2000"; }
  const char *GetDescString() { return "Yamaha DM2000"; }
  const char *GetConfigString() // string of configuration data
  {
    static char configtmp[512];
    if (m_ini_path[0])
        sprintf(configtmp, "%d %d %d %d %d %d %d %d %d %d|%s", m_midi_in_devs[0], m_midi_out_devs[0], m_midi_in_devs[1], m_midi_out_devs[1], m_midi_in_devs[2], m_midi_out_devs[2], m_midi_in_devs[3], m_midi_out_devs[3], m_general_in, m_general_out, m_ini_path);
    else
        sprintf(configtmp, "%d %d %d %d %d %d %d %d %d %d", m_midi_in_devs[0], m_midi_out_devs[0], m_midi_in_devs[1], m_midi_out_devs[1], m_midi_in_devs[2], m_midi_out_devs[2], m_midi_in_devs[3], m_midi_out_devs[3], m_general_in, m_general_out);
    return configtmp;
  }

  void CloseNoReset()
  {
      // HUI has no "goodbye" message -- the console flags DAW Off-line by itself
      // once the ping echo stops -- but clear our state so the remote layer does
      // not keep showing a frozen mix: meters off, LEDs off, pan rings cleared,
      // scribbles blanked, faders driven to -inf.
      for (int i = 0; i < 32; ++i)
      {
          if (m_midiouts[i / 8])
          {
              int ch = i & 7;
              m_midiouts[i / 8]->Send(0xB0, ch, 0, -1);             // fader MSB = 0 (-inf)
              m_midiouts[i / 8]->Send(0xB0, 0x20 + ch, 0, -1);      // fader LSB = 0
              m_midiouts[i / 8]->Send(0xA0, i & 7, 0x00, -1);       // meter L off
              m_midiouts[i / 8]->Send(0xA0, i & 7, 0x10, -1);       // meter R off
              m_midiouts[i / 8]->Send(0xB0, 0x10 + (i & 7), 0, -1); // pan ring off
          }
          SendChannelLED(i, 0, false); // fader-touch indicator
          SendChannelLED(i, 1, false);
          SendChannelLED(i, 2, true);  // [ON] LED is inverted on the DM2000 (lit = channel on),
                                       // so send "muted" to darken it for a clean shutdown
          SendChannelLED(i, 3, false);
          SendChannelLED(i, 4, false); // AUTO indicator
          SendChannelLED(i, 5, false); // V-pot LED / pan-ring blink state
          SendChannelLED(i, 6, false); // INS icon
          SendChannelLED(i, 7, false);
          SendTrackTitle(i, "");
      }
      SendTransportLED(3, false);
      SendTransportLED(4, false);
      SendTransportLED(5, false);
      SendCounter(true); // blank the LED counter display
      SendCounterMode(-1); // clear the TIME CODE / FEET / BEATS mode LEDs
      for (int w = 0; w < 8; ++w) SendRoutingLED(w, false); // clear MCS PANNER routing LEDs (port 4)
      for (int c = 0; c < 8; ++c) SendDisplayCell(c, "");   // blank the REMOTE INSERT EDIT text display
      SendSelectAssign("");                                 // blank the master SELECT ASSIGN ("Pan"/"SndA-E")
      if (m_midiouts[0]) for (int k = 0; k < 4; ++k) m_midiouts[0]->Send(0xB0, 0x18 + k, 0, -1); // EFFECTS SEL rings off
      for (int sw = 2; sw <= 7; ++sw) SendGlobalLED(0x0B, sw, false); // clear AUX/ENCODER-MODE button LEDs
      for (int sw = 0; sw <= 7; ++sw) SendGlobalLED(0x1C, sw, false); // clear EFFECTS/PLUG-INS indicator boxes
      SendAutomixLEDs(-1);                                  // clear AUTOMIX mode-button LEDs

      // REAPER's MIDI outputs queue internally (CreateThreadedMIDIOutput is a
      // passthrough on Windows); deleting them immediately drops whatever the
      // driver hasn't drained yet, leaving stale state on the console
      Sleep(200);

      for (int i = 0; i < 4; ++i)
      {
          delete m_midiouts[i];
          delete m_midiins[i];
          m_midiouts[i] = nullptr;
          m_midiins[i] = nullptr;
      }
      delete m_midiout8;
      m_midiout8 = nullptr;
      delete m_general_midiout;
      m_general_midiout = nullptr;
      delete m_general_midiin;
      m_general_midiin = nullptr;
  }

  void Run()
  {
      for (int i = 0; i < 4; ++i)
      {
          if (m_midiins[i])
          {
              m_midiins[i]->SwapBufs(timeGetTime());
              int l = 0;
              MIDI_eventlist *list = m_midiins[i]->GetReadBuf();
              MIDI_event_t *evts;
              while ((evts = list->EnumItems(&l))) OnMIDIEvent(evts, i);
          }
      }

      // GENERAL port: receive console scene recalls (Program Change) and jump to the
      // matching project marker. The scroll/display SysEx the console also emits is
      // ignored -- only the PC means "recall". scene N <-> marker N (PC = scene-1).
      if (m_general_midiin)
      {
          m_general_midiin->SwapBufs(timeGetTime());
          int gl = 0;
          MIDI_eventlist *glist = m_general_midiin->GetReadBuf();
          MIDI_event_t *gevt;
          while ((gevt = glist->EnumItems(&gl)))
              if (m_scene_receive && (gevt->midi_message[0] & 0xF0) == 0xC0)
              {
                  int scene = gevt->midi_message[1] + 1; // PC n -> scene n+1
                  // ignore the console's PC echo of a recall WE just drove (else the
                  // jump-to-marker would chase our own send and the cursor ping-pongs).
                  // Consume the matching ring entry so a genuine user re-recall of the
                  // same scene a moment later is NOT swallowed.
                  bool echo = false;
                  DWORD gnow = timeGetTime();
                  for (int e = 0; e < 8; ++e)
                      if (m_scene_echo[e] == scene && gnow - m_scene_echo_t[e] < 800)
                      { m_scene_echo[e] = -1; echo = true; break; }
                  if (echo) continue;
                  GotoSceneMarker(scene);
              }
      }

      PollSceneMarkers(); // GENERAL port send: drive the console as playback crosses #SCENE markers

      DWORD now = timeGetTime();

      // revert AUTO-mode strip labels once their ~1.2s flash expires
      for (int a = 0; a < 24; ++a)
          if (m_auto_label_time[a] && now - m_auto_label_time[a] >= 1200)
          { m_auto_label_time[a] = 0; RefreshScribble(a); }

      // revert the EFFECTS-DISPLAY "AutoWin" flash to the FX param display after ~1s
      if (m_winflash_time && now - m_winflash_time >= 1000)
      { m_winflash_time = 0; RefreshFXDisplay(); }

      // Direct (surround bank): a single press confirms as bank-up once the 400ms
      // double-click window passes with no second press.
      if (m_direct_pending && now - m_direct_time >= 400)
      {
          m_direct_pending = false;
          SetSurroundObject(m_surround_obj + 8);
      }

      // arrow auto-repeat: fire after 400ms hold, then every 80ms
      if (m_held_arrow >= 0 && now - m_arrow_held_since > 400)
      {
          if (now - m_arrow_last_repeat > 80)
          {
              CSurf_OnArrow(m_held_arrow, m_arrow_mode == 1); // NAVIGATION (scroll) or ZOOM
              m_arrow_last_repeat = now;
          }
      }

      // REW/FF auto-repeat: same timing as arrows
      if (m_held_transport >= 0 && now - m_transport_held_since > 400)
      {
          if (now - m_transport_last_repeat > 80)
          {
              if (m_held_transport == 1) CSurf_OnRew(1);
              else                       CSurf_OnFwd(1);
              m_transport_last_repeat = now;
          }
      }

      // LED counter: refresh faster than the 100ms meter poll for smooth SMPTE
      // frames. SendCounter() only emits MIDI for digits that actually changed, so
      // a fast tick costs a string format + compare and stays silent when stopped.
      if (now - m_tc_lastrun >= (DWORD)m_tc_interval)
      {
          m_tc_lastrun = now;
          SendCounter();
      }

      if (now - m_meter_lastrun >= 100) // unsigned diff also handles timer wrap
      {
          m_meter_lastrun = now;
          if (now - m_blink_last >= 400) { m_blink_last = now; m_blink_phase = !m_blink_phase; } // ~1.25 Hz blink

          // Startup splash: ~1s after load (desk now online), blink "REAPER online" ~3x on the REMOTE
          // display, then hand off to the FX view - clears the console's "Off-Line / waiting for MIDI" text.
          if (!m_splash_done)
          {
              if (!m_splash_start) m_splash_start = now;
              DWORD age = now - m_splash_start;
              if (age >= 1000 && !m_splash_render)             // ~1s after load (desk online): paint the card once
              {
                  m_splash_render = 1;
                  for (int c = 0; c < 8; ++c) SendDisplayCell(c, "");
                  SendDisplayCell(0, "DM2000");  SendDisplayCell(1, "csurf");      // top: DM2000 csurf online vX.Y
                  SendDisplayCell(2, "online");  SendDisplayCell(3, DM2000_CSURF_VERSION);
                  SendDisplayCell(4, "bmroz.eu/p"); SendDisplayCell(5, "rojects/dm"); // bottom: project website,
                  SendDisplayCell(6, "2000-csurf");                                   //   sliced across abutting cells
              }
              else if (age >= 1000 + 2500) { m_splash_done = 1; RefreshFXDisplay(); } // ~2.5s, then hand off to FX view
          }
          PollRoutingLEDs(); // refresh surround routing LEDs on selection/object/plugin change
          PollSendRings();   // live-refresh encoder send-level rings while in send mode
          m_meter_histpos = (m_meter_histpos + 1) % 3;
          for (int i = 0; i < 32; ++i)
          {
              if (!m_midiouts[i / 8]) continue;

              // trackless channels fall through with level 0 so their meter clears
              unsigned char lvlL = 0, lvlR = 0;
              MediaTrack *tr = TrackFromCh(i);
              if (tr)
              {
                  lvlL = peakToMeter(Track_GetPeakInfo(tr, 0));
                  lvlR = peakToMeter(Track_GetPeakInfo(tr, 1));
              }

              // peak hold: send the max of the last 3 polls to steady the display
              m_meter_hist[i][0][m_meter_histpos] = lvlL;
              m_meter_hist[i][1][m_meter_histpos] = lvlR;
              for (int h = 0; h < 3; ++h)
              {
                  if (m_meter_hist[i][0][h] > lvlL) lvlL = m_meter_hist[i][0][h];
                  if (m_meter_hist[i][1][h] > lvlR) lvlR = m_meter_hist[i][1][h];
              }

              unsigned char packed = (unsigned char)((lvlL << 4) | lvlR);
              if (m_meter_lastlvl[i] != packed)
              {
                  m_meter_lastlvl[i] = packed;
                  // HUI meters: poly key pressure, value = (side << 4) | level
                  m_midiouts[i / 8]->Send(0xA0, i & 7, lvlL, -1);        // left
                  m_midiouts[i / 8]->Send(0xA0, i & 7, 0x10 | lvlR, -1); // right
              }

              // per-channel status icons (HUI channel zone, real channels 0-23 only):
              // INS (sw6) = track has FX; AUTO (sw4) = track armed for writing (touch/write/latch)
              if (i < 24)
              {
                  char ins = (m_show_ins && tr && TrackFX_GetCount && TrackFX_GetCount(tr) > 0) ? 1 : 0;
                  if (m_ins_state[i] != ins) { m_ins_state[i] = ins; SendChannelLED(i, 6, ins != 0); }
                  // AUTO indicator is multi-colour (sw4): green=Read, orange=Touch/Latch, red=Write.
                  // Write (red) and Latch (orange) blink on the dark phase to flag active-write /
                  // latch-armed; Touch (also orange) stays solid, so the blink tells Touch from Latch.
                  unsigned char acol = AutoColorByte(tr);
                  int amode = GetTrackAutomationMode ? GetTrackAutomationMode(tr) : -1;
                  if (!m_blink_phase && (acol == 0x44 || amode == AUTO_MODE_LATCH)) acol = 0x04;
                  if ((unsigned char)m_auto_state[i] != acol) { m_auto_state[i] = (char)acol; SendChannelLEDVal(i, acol); }
              }
          }
      }
  }

  void SetTrackListChange()
  {
      // surface channels past the last track must not keep a previous track's
      // scribble or LEDs: banking can otherwise strand e.g. a lit SOLO/SELECT on
      // an empty channel (the host only repaints channels that map to a track).
      for (int i = 0; i < 32; ++i)
          if (!TrackFromCh(i))
          {
              SendTrackTitle(i, "");
              SendChannelLED(i, 1, false); // SELECT
              SendChannelLED(i, 2, true);  // [ON] inverted -> send "muted" to darken
              SendChannelLED(i, 3, false); // SOLO
              SendChannelLED(i, 5, false); // V-pot / pan-ring
              SendChannelLED(i, 7, false); // REC
              SendChannelLED(i, 4, false); m_auto_state[i] = 0; // AUTO indicator
              SendChannelLED(i, 6, false); m_ins_state[i] = 0;  // INS icon
              if (m_midiouts[i / 8]) m_midiouts[i / 8]->Send(0xB0, 0x10 + (i & 7), 0, -1); // pan ring position off
              m_pan_lastpos[i] = -1; // invalidate cache so a reappearing track repaints its ring even at the same pan
          }
  }
  void SetSurfaceVolume(MediaTrack *trackid, double volume)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

      int volint = volToInt14(volume);
      if (m_vol_lastpos[id] != volint)
      {
          m_vol_lastpos[id] = volint;
          int ch = id & 7;
          if (flipSends() && id < 24)        // FLIP: volume rides the V-pot ring, not the motor fader
          {
              m_midiouts[id / 8]->Send(0xB0, 0x10 + ch, (unsigned char)(1 + (volint * 10) / 16383), -1);
          }
          else
          {
              m_midiouts[id / 8]->Send(0xB0, ch, (volint >> 7) & 0x7F, -1);
              m_midiouts[id / 8]->Send(0xB0, 0x20 + ch, volint & 0x7F, -1);
          }
          // live-update the strip when it is showing dB (held peek, or this fader is touched)
          if (id < 24 && (m_scribble_peek == 2 || (m_touch_db && m_fader_touch[id]))) RefreshScribble(id);
      }
  }
  void SetSurfacePan(MediaTrack *trackid, double pan)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

      unsigned char panch = panToChar(pan);
      if (m_pan_lastpos[id] != panch)
      {
          m_pan_lastpos[id] = panch;
          if (m_enc_send < 0)  // pan mode owns the ring; in send mode the ring shows send level
              m_midiouts[id / 8]->Send(0xB0, 0x10 + (id & 7), 1 + ((panch * 11) >> 7), -1); // ring LED 1-11
      }
  }
  void SetSurfaceMute(MediaTrack *trackid, bool mute)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 2, mute);
  }
  void SetSurfaceSelected(MediaTrack *trackid, bool selected)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 1, selected);
      if (selected)            // a newly selected track drives the FX editor: show it from the top
      {
          m_fx_slot = 0; m_fx_page = 0;
          RefreshFXDisplay();
      }
  }
  void SetSurfaceSolo(MediaTrack *trackid, bool solo)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 3, solo);
  }
  void SetSurfaceRecArm(MediaTrack *trackid, bool recarm)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 7, recarm);
  }
  void SetPlayState(bool play, bool pause, bool rec)
  {
      SendTransportLED(3, !play && !pause); // STOP
      SendTransportLED(4, play);            // PLAY
      SendTransportLED(5, rec);             // REC
  }
  void SetRepeatState(bool rep) { SendGlobalLED(0x0F, 3, rep); } // LOOP is zone 0x0F sw3 (hw-verified 2026-06-15)

  void SetTrackTitle(MediaTrack *trackid, const char *title)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 24)                  // cache the name so overrides can restore it
      {
          int j; for (j = 0; j < 7 && title && title[j]; ++j) m_scribble_name[id][j] = title[j];
          m_scribble_name[id][j] = 0;
      }
      if (m_scribble_peek == 0 && m_enc_send < 0) // no scribble override active: show the name now
          SendTrackTitle(id, title);
  }
  bool GetTouchState(MediaTrack *trackid, int isPan)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32) return false;
      if (isPan == 1)
      {
          DWORD now = timeGetTime();
          // fake touch, go for 3s after last knob movement
          return now < m_pan_lasttouch[id] + 3000 && now >= m_pan_lasttouch[id] - 1000;
      }
      return !!m_fader_touch[id];
  }
  void SetAutoMode(int mode)
  {
      m_auto_mode = mode;
      SendAutomixLEDs(mode);
  }
  void ResetCachedVolPanStates()
  {
      memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
      memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
  }
  void OnTrackSelection(MediaTrack *trackid)
  {
      // FX editor follows the selected track: reset to its first slot / first page
      m_fx_slot = 0;
      m_fx_page = 0;
  }

  bool IsKeyDown(int key) { return false; }

private:
    // surface channel (0..23) -> REAPER track, skipping master (track id 0)
    MediaTrack *TrackFromCh(int gch)
    {
        return CSurf_TrackFromID(m_bank_offset + gch + 1, false);
    }

    // Fold a Unicode code point to a base ASCII letter for the scribble strips, which
    // drop any byte >= 0x80 (so "Łóżko" otherwise shows as just "k"). Covers Polish +
    // Latin-1 accented letters; returns 0 to drop an unmapped non-ASCII character.
    static char foldCp(unsigned int cp)
    {
        switch (cp)
        {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: return 'A';
            case 0xC7: return 'C';
            case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
            case 0xD0: return 'D';
            case 0xD1: return 'N';
            case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
            case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
            case 0xDD: return 'Y';
            case 0xDF: return 's';                                          // ß
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: return 'a';
            case 0xE7: return 'c';
            case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
            case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
            case 0xF1: return 'n';
            case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
            case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
            case 0xFD: case 0xFF: return 'y';
            case 0x104: return 'A'; case 0x105: return 'a';                 // Ą ą
            case 0x106: return 'C'; case 0x107: return 'c';                 // Ć ć
            case 0x118: return 'E'; case 0x119: return 'e';                 // Ę ę
            case 0x141: return 'L'; case 0x142: return 'l';                 // Ł ł
            case 0x143: return 'N'; case 0x144: return 'n';                 // Ń ń
            case 0x15A: return 'S'; case 0x15B: return 's';                 // Ś ś
            case 0x179: return 'Z'; case 0x17A: return 'z';                 // Ź ź
            case 0x17B: return 'Z'; case 0x17C: return 'z';                 // Ż ż
            default: return 0;                                              // unmapped -> drop
        }
    }

    // UTF-8 -> ASCII fold for the scribble strips (the desk drops bytes >= 0x80, so
    // accented / Polish track names would otherwise vanish). Decodes 1-3 byte UTF-8.
    static void scribbleAsciiFold(const char *src, char *dst, int dstsize)
    {
        int o = 0;
        const unsigned char *p = (const unsigned char *)src;
        while (*p && o < dstsize - 1)
        {
            unsigned int cp;
            if (*p < 0x80) cp = *p++;
            else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
                { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); p += 2; }
            else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
                { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); p += 3; }
            else { ++p; continue; }                                         // invalid byte - skip
            char a = (cp < 0x80) ? (char)cp : foldCp(cp);
            if (a) dst[o++] = a;
        }
        dst[o] = 0;
    }

    // HUI channel scribble strip, same byte format as csurf_babyhui.cpp:
    // F0 00 00 66 05 00 10 <ch> <4 chars> F7
    void SendTrackTitle(int id, const char *title)
    {
        if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

        char name[16];
        scribbleAsciiFold(title ? title : "", name, sizeof(name));   // UTF-8 (e.g. Polish) -> ASCII
        int len = (int)strlen(name);

        unsigned char sysex[13] = { 0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7 };
        sysex[7] = id & 7;
        for (int i = 0; i < 4 && i < len; ++i)
            sysex[8 + i] = name[i] & 0x7F;

        char buf[sizeof(MIDI_event_t) + sizeof(sysex)];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = sizeof(sysex);
        memcpy(msg->midi_message, sysex, sizeof(sysex));
        m_midiouts[id / 8]->SendMsg(msg, -1);

        // Layer 3: native channel name on port 8, hardware-captured 2026-06-12.
        // Studio Manager sends: F0 43 17 3E 06 02 04 [pos] [ch] 00 00 00 [ASCII] F7
        // Studio Manager used pos=0..3; we send 0..7 to probe 8-char display support.
        if (m_midiout8)
        {
            for (int pos = 0; pos < 8; ++pos)
            {
                unsigned char c = (pos < len) ? (name[pos] & 0x7F) : 0x20;
                unsigned char buf[] = { 0xF0, 0x43, 0x17, 0x3E, 0x06, 0x02, 0x04,
                                        (unsigned char)pos, (unsigned char)(id & 0x7F),
                                        0x00, 0x00, 0x00, c, 0xF7 };
                SendSysEx(buf, sizeof(buf));
            }
        }
    }

    // HUI counter (LED timecode) display.
    // Protocol captured 2026-06-15 from Pro Tools via loopMIDI on port 1:
    //   F0 00 00 66 05 00 11 [N bytes right-to-left] F7
    // Each byte: high nibble = 1 if a separator (. or :) follows that digit in the
    // display; low nibble = BCD digit 0-9. Only positions that changed since the
    // previous message are sent, spanning from the rightmost to leftmost change.
    // Special clear: 8 x 0x20 blanks all positions (used at init and on close).
    // Counter-mode indicator LEDs (zone 0x16, hw-captured from Pro Tools 2026-06-17):
    // sw0 = TIME CODE, sw1 = FEET, sw2 = BEATS. Driven from REAPER's transport display
    // mode, mirroring csurf_mcu.cpp's SMPTE/BEATS lights: tmode 5 = h:m:s:f (timecode),
    // tmode 1-2 = measures/beats. (REAPER has no "feet" mode, so FEET stays dark.)
    // Counter-mode indicator LEDs (zone 0x16). REAPER packs BOTH time units into tmode:
    // the LOW byte is the primary display, the secondary unit rides in the high bits
    // (hw-harvested 2026-06-17). Mask to the low byte, then group every subtype onto the
    // right indicator. Primary values: 0=Min:Sec 11=Min:Sec(min) 3=Seconds 5=H:M:S:F |
    // 2=Meas.Beats 6=Meas.Beats(min) 10=Meas.Fractions (1/7 = the same two under a
    // Min:Sec secondary) | 4=Samples 8=Absolute Frames. tmode -1 (clear) masks to 0xFF
    // and matches nothing, so all LEDs go off.
    void SendCounterMode(int tmode)
    {
        int prim = tmode & 0xFF;
        bool beats = (prim == 2 || prim == 6 || prim == 10 || prim == 1 || prim == 7);
        bool feet  = (prim == 4 || prim == 8);
        bool tcode = (prim == 0 || prim == 3 || prim == 5 || prim == 11);
        SendGlobalLED(0x16, 0, tcode);   // TIME CODE  (sw0, hw-confirmed 2026-06-18)
        SendGlobalLED(0x16, 1, feet);    // FEET       (sw1, hw-confirmed 2026-06-18)
        SendGlobalLED(0x16, 2, beats);   // BEATS      (sw2, hw-confirmed)
    }

    void SendCounter(bool clear = false)
    {
        if (!m_midiouts[0]) return;

        if (clear)
        {
            unsigned char blank[17] = {
                0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11,
                0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                0xF7
            };
            char msgbuf[sizeof(MIDI_event_t) + 17];
            MIDI_event_t *msg = (MIDI_event_t *)msgbuf;
            msg->frame_offset = -1;
            msg->size = 17;
            memcpy(msg->midi_message, blank, 17);
            m_midiouts[0]->SendMsg(msg, -1);
            memset(m_tc_lastbuf, 0x20, sizeof(m_tc_lastbuf));
            return;
        }

        // Read transport display mode: prefer timemode2 (right-click transport setting)
        // over timemode (project primary), same logic as csurf_mcu.cpp.
        int tmode = 0;
        int *tmodeptr = (int *)projectconfig_var_addr(NULL, __g_projectconfig_timemode2);
        if (tmodeptr && *tmodeptr >= 0) tmode = *tmodeptr;
        else {
            tmodeptr = (int *)projectconfig_var_addr(NULL, __g_projectconfig_timemode);
            if (tmodeptr) tmode = *tmodeptr;
        }

        // drive the counter-mode indicator LEDs (zone 0x16) when the format changes
        if (m_tc_lastmode != tmode)
        {
            m_tc_lastmode = tmode;
            SendCounterMode(tmode);
            // [debug] console: log the REAPER time-display value so all subtypes can be
            // grouped onto the right TIME CODE / FEET / BEATS indicator (harvest each mode's number).
            if (m_console_log && ShowConsoleMsg)
            { char b[48]; sprintf_s(b, sizeof(b), "[DM2000] counter tmode = %d\n", tmode); ShowConsoleMsg(b); }
        }

        double pos = (GetPlayState() & 1) ? GetPlayPosition() : GetCursorPosition();
        char tbuf[64];
        format_timestr_pos(pos, tbuf, sizeof(tbuf), tmode);

        // Parse the time string right-to-left into 8 display positions.
        // Separator chars (: . ,) mark the digit to their left as having a separator after it.
        unsigned char disp[8];
        memset(disp, 0x20, sizeof(disp)); // 0x20 = blank position

        int slen = (int)strlen(tbuf);
        int dpos = 7;
        bool sep_follows = false;
        for (int i = slen - 1; i >= 0 && dpos >= 0; --i)
        {
            char c = tbuf[i];
            if (c >= '0' && c <= '9')
            {
                disp[dpos--] = (unsigned char)((sep_follows ? 0x10 : 0x00) | (c - '0'));
                sep_follows = false;
            }
            else if (c == '.' || c == ':' || c == ',')
            {
                sep_follows = true;
            }
        }

        // Find the leftmost and rightmost positions that changed.
        int lmc = -1, rmc = -1;
        for (int i = 0; i < 8; ++i)
        {
            if (disp[i] != m_tc_lastbuf[i])
            {
                if (lmc < 0) lmc = i;
                rmc = i;
            }
        }
        if (lmc < 0) return;

        memcpy(m_tc_lastbuf, disp, 8);

        unsigned char sysex[16]; // 7 header + up to 8 data + 1 F7
        int si = 0;
        sysex[si++] = 0xF0; sysex[si++] = 0x00; sysex[si++] = 0x00;
        sysex[si++] = 0x66; sysex[si++] = 0x05; sysex[si++] = 0x00;
        sysex[si++] = 0x11;
        for (int i = rmc; i >= lmc; --i)
            sysex[si++] = disp[i];
        sysex[si++] = 0xF7;

        char msgbuf[sizeof(MIDI_event_t) + 16];
        MIDI_event_t *msg = (MIDI_event_t *)msgbuf;
        msg->frame_offset = -1;
        msg->size = si;
        memcpy(msg->midi_message, sysex, si);
        m_midiouts[0]->SendMsg(msg, -1);
    }

    // native Yamaha SysEx out on USB port 8 (Layer 3 features: 8-char names,
    // meter bridge, scene recall); data must be a complete F0..F7 message
    void SendSysEx(const unsigned char *data, int len)
    {
        if (!m_midiout8 || len < 2 || len > 200) return;

        char buf[sizeof(MIDI_event_t) + 200];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = len;
        memcpy(msg->midi_message, data, len);
        m_midiout8->SendMsg(msg, -1);
    }

    // ---- Scene recall over the GENERAL port -------------------------------------
    // case-insensitive prefix test with a trailing word boundary, so "#SCENERY" does
    // not match "#SCENE". On a match, *out_rest points just past the prefix.
    static bool MatchScenePrefix(const char *name, const char *prefix, const char **out_rest)
    {
        if (!prefix || !prefix[0]) return false;
        int pl = (int)strlen(prefix);
        if (strnicmp(name, prefix, pl) != 0) return false;
        char nx = name[pl];
        if (nx != 0 && nx != ' ' && nx != '\t' && !(nx >= '0' && nx <= '9')) return false;
        *out_rest = name + pl;
        return true;
    }

    // Match a marker name against the scene prefix. The scene number is the first
    // token after the prefix; if it is absent or non-numeric, the marker's own number
    // (mnum) is used. Anything after the number is a free label and ignored, e.g.
    // "#SCENE 4 Chorus" -> scene 4. Returns false for non-scene markers.
    bool ParseSceneMarker(const char *name, int mnum, int *scene)
    {
        if (!name) return false;
        const char *rest = NULL;
        if (!MatchScenePrefix(name, m_scene_prefix, &rest)) return false;
        while (*rest == ' ' || *rest == '\t') ++rest;
        *scene = (*rest >= '0' && *rest <= '9') ? atoi(rest) : mnum;
        return (*scene >= 1 && *scene <= 99); // scene 0 / out-of-range: not a usable scene marker
    }

    // Scroll/display message: sets the console's pending scene number WITHOUT
    // recalling. Mirrors what the DM2000 emits while turning its scene dial:
    //   F0 43 10 3E 06 04 0A 00 00 00 00 00 <scene+1> F7   (hw-captured 2026-06-16)
    void SendSceneDisplay(int scene)
    {
        if (!m_general_midiout || scene < 0 || scene > 99) return;
        unsigned char sx[14] = { 0xF0, 0x43, 0x10, 0x3E, 0x06, 0x04, 0x0A,
                                 0x00, 0x00, 0x00, 0x00, 0x00,
                                 (unsigned char)(scene + 1), 0xF7 };
        char buf[sizeof(MIDI_event_t) + 14];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = 14;
        memcpy(msg->midi_message, sx, 14);
        m_general_midiout->SendMsg(msg, -1);
    }

    // Recall: the DM2000 emits Program Change (scene-1) when RECALL is pressed, so we
    // send the same to make the console load the scene. scenes 1-99 (PC 0-98).
    void SendSceneRecall(int scene)
    {
        if (!m_general_midiout || scene < 1 || scene > 99) return;
        m_general_midiout->Send(0xC0, (unsigned char)(scene - 1), 0, -1);
        // arm echo suppression: the console PCs every recall back; a ring (not a single
        // scalar) tracks a whole burst so each in-flight echo is matched exactly once.
        m_scene_echo[m_scene_echo_pos] = scene;
        m_scene_echo_t[m_scene_echo_pos] = timeGetTime();
        m_scene_echo_pos = (m_scene_echo_pos + 1) % 8;
    }

    // Receive: a console scene recall moves the REAPER cursor to the marker that
    // represents that scene. To round-trip cleanly with the SEND side (which derives
    // the scene from a "#SCENE N" token), prefer a scene marker whose parsed number
    // matches; fall back to a plain marker numbered N (the Locate "go to marker N"
    // convenience). No-op if neither exists.
    void GotoSceneMarker(int scene)
    {
        if (scene < 1 || scene > 99 || !EnumProjectMarkers || !SetEditCurPos) return;
        double tagged = -1.0, numbered = -1.0;
        int idx = 0;
        bool isrgn; double mpos; const char *mname; int mnum;
        while ((idx = EnumProjectMarkers(idx, &isrgn, &mpos, NULL, &mname, &mnum)))
        {
            if (isrgn) continue;
            int s;
            if (ParseSceneMarker(mname, mnum, &s) && s == scene) { tagged = mpos; break; } // exact #SCENE match wins
            if (mnum == scene && numbered < 0.0) numbered = mpos;                          // plain marker numbered N
        }
        double pos = (tagged >= 0.0) ? tagged : numbered;
        if (pos < 0.0) return;
        SetEditCurPos(pos, true, (GetPlayState() & 1) != 0); // seek too if playing
        // Record the scene the console just recalled so the send/follow de-dup won't
        // echo this same recall straight back to the desk a moment later.
        m_scene_applied = scene;
    }

    // Scene that owns a timeline position: the latest scene marker at or before it
    // (false if none). Shared by the playback resync and the stopped display path.
    bool OwningScene(double atpos, int *scene)
    {
        int own = -1; double best = -1.0;
        int idx = 0;
        bool isrgn; double mpos; const char *mname; int mnum;
        while ((idx = EnumProjectMarkers(idx, &isrgn, &mpos, NULL, &mname, &mnum)))
        {
            if (isrgn || mpos > atpos) continue;
            int s;
            if (ParseSceneMarker(mname, mnum, &s) && mpos >= best) { best = mpos; own = s; }
        }
        if (own < 0) return false;
        *scene = own; return true;
    }

    // Send: drive the console's scene section from "#SCENE n" project markers.
    //   PLAYING  -> RECALL the scene as the play cursor crosses its marker (and on
    //               play-start / loop-wrap / seek, resync to the scene owning the new
    //               position). This physically reloads the desk in time with the music.
    //   STOPPED  -> DISPLAY only: once the edit cursor settles, scroll the console's
    //               scene number to the scene owning the cursor - never recalls while
    //               stopped (so editing never sweeps the faders, and there is no recall
    //               -> PC-echo -> jump feedback loop).
    // m_scene_applied de-dupes so the desk is driven only when the scene actually
    // changes - no snapshot reload on every loop pass or nudge.
    void PollSceneMarkers()
    {
        if (!m_scene_send || !m_general_midiout || !EnumProjectMarkers)
        {
            m_scene_lastpos = -1.0; m_scene_applied = -1; return;
        }

        if (GetPlayState() & 1)
        {
            double pos = GetPlayPosition();
            if (m_scene_lastpos < 0.0 || pos < m_scene_lastpos)
            {
                // play just started, or the play cursor jumped backwards (loop wrap or
                // seek): resync to (recall) the scene that owns the new position, de-duped.
                int scene;
                if (OwningScene(pos, &scene) && scene != m_scene_applied)
                {
                    SendSceneDisplay(scene);
                    SendSceneRecall(scene);
                    m_scene_applied = scene;
                }
            }
            else if (pos > m_scene_lastpos)
            {
                // forward crossing: recall every scene marker in (lastpos, pos], in order
                int idx = 0;
                bool isrgn; double mpos; const char *mname; int mnum;
                while ((idx = EnumProjectMarkers(idx, &isrgn, &mpos, NULL, &mname, &mnum)))
                {
                    if (isrgn || mpos <= m_scene_lastpos || mpos > pos) continue;
                    int scene;
                    if (ParseSceneMarker(mname, mnum, &scene))
                    {
                        SendSceneDisplay(scene);
                        SendSceneRecall(scene);
                        m_scene_applied = scene; // carry into stop so we don't re-recall on stop
                    }
                }
            }
            m_scene_lastpos = pos;
            m_scene_cursorpos = -1.0; // re-arm the stopped settle timer
            return;
        }

        // stopped: scroll the display to the owning scene once the cursor settles
        m_scene_lastpos = -1.0;            // re-arm playback crossing detection

        double cur = GetCursorPosition();
        DWORD now = timeGetTime();
        if (cur != m_scene_cursorpos) { m_scene_cursorpos = cur; m_scene_cursortime = now; return; }
        if (now - m_scene_cursortime < 300) return; // wait for the cursor to settle

        int scene;
        if (OwningScene(cur, &scene) && scene != m_scene_applied)
        {
            m_scene_applied = scene;
            SendSceneDisplay(scene); // display only - no recall while stopped
        }
    }

    // host->surface switch LED: zone select on 0x0C, switch|state on 0x2C
    void SendChannelLED(int id, int sw, bool state)
    {
        if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;
        m_midiouts[id / 8]->Send(0xB0, 0x0C, id & 7, -1);
        m_midiouts[id / 8]->Send(0xB0, 0x2C, (state ? 0x40 : 0x00) | sw, -1);
    }

    // Channel LED with a raw value byte (for multi-state LEDs like the colour AUTO
    // indicator on sw4: 0x04 off, 0x34 green, 0x64 orange, 0x44 red - hw-captured).
    void SendChannelLEDVal(int id, unsigned char val)
    {
        if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;
        m_midiouts[id / 8]->Send(0xB0, 0x0C, id & 7, -1);
        m_midiouts[id / 8]->Send(0xB0, 0x2C, val, -1);
    }

    // AUTO indicator (sw4) colour for a track's automation mode, mirroring Pro Tools:
    // Read=green, Touch/Latch=orange, Write=red, Trim/off=dark.
    unsigned char AutoColorByte(MediaTrack *tr)
    {
        if (!m_show_auto || !tr || !GetTrackAutomationMode) return 0x04;
        switch (GetTrackAutomationMode(tr))
        {
            case AUTO_MODE_READ:  return 0x34; // green
            case AUTO_MODE_TOUCH: return 0x64; // orange
            case AUTO_MODE_LATCH: return 0x64; // orange
            case AUTO_MODE_WRITE: return 0x44; // red
            default:              return 0x04; // trim / off
        }
    }

    // LED in a global (non-channel) zone; these controls live on the first HUI unit (port 1)
    void SendGlobalLED(int zone, int sw, bool state)
    {
        if (!m_midiouts[0]) return;
        m_midiouts[0]->Send(0xB0, 0x0C, zone, -1);
        m_midiouts[0]->Send(0xB0, 0x2C, (state ? 0x40 : 0x00) | sw, -1);
    }

    void SendTransportLED(int sw, bool state) { SendGlobalLED(0x0E, sw, state); }

    // Light the AUTOMIX mode button (zone 0x18) matching `mode`; clear the rest.
    // Verified map (2026-06-17): TRIM=sw0(RELATIVE) LATCH=sw1(AUTO-REC) READ=sw2(RETURN)
    //                            TOUCH=sw5(ABORT/UNDO) WRITE=sw4(REC). [TOUCH SENSE sw3 = Off]
    void SendAutomixLEDs(int mode)
    {
        static const int sw_for[5] = { 0, 2, 5, 4, 1 }; // AUTO_MODE_TRIM/READ/TOUCH/WRITE/LATCH -> zone 0x18 switch
        int lit_sw = (mode >= 0 && mode <= 4) ? sw_for[mode] : -1;
        for (int p = 0; p < 3; ++p)
        {
            if (!m_midiouts[p]) continue;
            for (int s = 0; s <= 5; ++s)
            {
                m_midiouts[p]->Send(0xB0, 0x0C, 0x18, -1);
                m_midiouts[p]->Send(0xB0, 0x2C, (s == lit_sw ? 0x40 : 0x00) | s, -1);
            }
        }
    }

    void OnMIDIEvent(MIDI_event_t *evt, int port)
    {
        unsigned char status = evt->midi_message[0] & 0xF0;
        unsigned char data1  = evt->midi_message[1];
        unsigned char data2  = evt->midi_message[2];

        // Scene recall (Program Change) arrives on the dedicated GENERAL port, polled
        // separately in Run() -- the 4 DAW/HUI ports here carry no scene traffic.

        // HUI keepalive ping - must echo back or surface goes offline
        if (status == 0x90 && data1 == 0x00 && data2 == 0x7F)
        {
            if (m_midiouts[port])
                m_midiouts[port]->Send(0x90, 0x00, 0x7F, -1);
            return;
        }

        if (status != 0xB0) return;

        // Port 4 (index 3) = MCS PANNER: dynamics knobs + joystick drive the surround
        // plugin on the selected track; ROUTING [6] toggles mode. Intercept before the
        // generic fader handlers so this traffic never moves phantom faders.
        if (port == 3 && OnPannerCC(data1, data2)) return;

        if (data1 == 0x0F)                       // switch matrix: zone select
        {
            m_zone[port] = data2;
        }
        else if (data1 == 0x2F)                  // switch matrix: switch + state in current zone
        {
            OnSwitch(port, m_zone[port], data2);
        }
        else if (data1 < 0x08)                   // fader value MSB, channel 0-7 of this port
        {
            m_fader_msb[port][data1] = data2;
        }
        else if (data1 >= 0x20 && data1 < 0x28)  // fader value LSB -> apply the move
        {
            int ch = data1 - 0x20;
            int gch = port * 8 + ch;
            MediaTrack *tr = TrackFromCh(gch);
            double gain = int14ToVol(m_fader_msb[port][ch], data2);
            if (tr && flipSends() && gch < 24)   // FLIP: the fader rides the selected send level
            {
                if (GetTrackNumSends && SetTrackSendInfo_Value && m_enc_send < GetTrackNumSends(tr, 0))
                    SetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL", gain);
                // echo the position back so the desk updates its internal fader model and does
                // not spring the motor back on release (same reason the volume path echoes)
                SendFlipFader(gch, true);
            }
            else if (tr)
                // ignoresurf=NULL on purpose: the DM2000 keeps an internal model of
                // the DAW's fader positions and springs the motor back to it on touch
                // release, so our own moves must be echoed back (Pro Tools does this)
                CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, gain, false), NULL);
        }
        else if (data1 == 0x0D)                  // jog wheel: bits 0-5 = speed (1-6), bit 6 = direction
        {
            // Direction verified on hardware: bit 6 SET moves forward (the initial report had it reversed)
            int speed = data2 & 0x3F;
            if (speed)
            {
                double dir = (data2 & 0x40) ? 1.0 : -1.0;
                if (m_wheel_mode == 1)      CSurf_ScrubAmt(dir * speed * 0.5);        // SCRUB: audible scrub
                else if (m_wheel_mode == 2) MoveEditCursor(dir * speed * 1.0, false); // SHUTTLE: coarse, 1s/click
                else                        MoveEditCursor(dir * speed * 0.1, false); // jog: 0.1s/click
            }
        }
        else if (data1 >= 0x40 && data1 < 0x48)  // channel encoder delta: pan (default) or send level
        {
            int gch = port * 8 + (data1 - 0x40);
            if (m_enc_send >= 0)                  // AUX/send mode
            {
                if (m_flip) OnVolumeKnob(gch, data2);  // FLIP: the encoder rides volume
                else        OnSendKnob(gch, data2);    // normal: the encoder rides the send
            }
            else                                 // pan v-pot delta: bits 0-5 = amount, bit 6 = right
            {
                double adj = (data2 & 0x3F) / -63.0;
                if (data2 & 0x40) adj = -adj;
                MediaTrack *tr = TrackFromCh(gch);
                if (tr) CSurf_SetSurfacePan(tr, CSurf_OnPanChange(tr, adj, true), NULL);
                m_pan_lasttouch[gch] = timeGetTime();
            }
        }
        else if (data1 >= 0x48 && data1 <= 0x4C && port == 0) // FX editor: param knobs 1-4 + page arrows
        {
            if (data1 == 0x4C) OnFXPage(relStep(data2)); // dedicated page encoder
            else               OnParamKnob(data1 - 0x48, data2);
        }
    }

    // SELECT button (channel sw1). Default ([select] exclusive = 1) is Pro Tools-style:
    // a single press selects ONLY that track; to select several, hold one SEL and press
    // the others (m_sel_held tracks what's physically down). [select] exclusive = 0 keeps
    // REAPER's additive toggle (each press adds/removes that track).
    void OnSelectButton(int gch, bool press)
    {
        m_sel_held[gch] = press ? 1 : 0;
        if (!press) return;
        MediaTrack *tr = TrackFromCh(gch);
        if (!tr) return;

        bool chord = false;                       // another SEL already held -> add to selection
        for (int i = 0; i < 32; ++i) if (i != gch && m_sel_held[i]) { chord = true; break; }

        if (m_sel_exclusive && !chord)
        {
            // exclusive: clear every other track's selection, then select this one
            int n = CSurf_NumTracks(false);
            for (int i = 0; i <= n; ++i)          // i=0 is the master track
            {
                MediaTrack *t = CSurf_TrackFromID(i, false);
                if (t && t != tr) CSurf_OnSelectedChange(t, 0);
            }
            CSurf_OnSelectedChange(tr, 1);
            // drive the surface SEL LEDs directly: only tr's channel is now selected
            for (int i = 0; i < 24; ++i) { MediaTrack *t = TrackFromCh(i); SendChannelLED(i, 1, t == tr); }
        }
        else
        {
            CSurf_SetSurfaceSelected(tr, CSurf_OnSelectedChange(tr, -1), NULL); // additive toggle
        }
    }

    // ---- Channel-encoder "send level" mode (ch.19: AUX SELECT = Sends A-E) ----------
    // The channel encoders normally ride pan; AUX SELECT [1-5] switches them to control
    // send 1-5 instead (m_enc_send 0-4), ENC PAN returns to pan (-1). The v-pot ring shows
    // the value either way (pan position, or send level 1-11).

    // paint one channel's encoder ring for the current mode (pan or send), plus its V-pot
    // ring-blink (sw5): while encoders ride a send, blink the channels that actually carry it
    // (the blinking ring signals "this ring isn't pan"). The blink is cached so the live poll
    // only toggles it on change - no stutter - and a send added/removed mid-session updates it.
    void SendEncoderRing(int gch, bool force = true)
    {
        if (gch < 0 || gch >= 24 || !m_midiouts[gch / 8]) return;
        MediaTrack *tr = TrackFromCh(gch);
        unsigned char ring = 0;                                  // 0 = ring off
        bool has_send = false;
        if (tr)
        {
            if (m_enc_send < 0)                                  // pan
            {
                double vol, pan;
                if (GetTrackUIVolPan && GetTrackUIVolPan(tr, &vol, &pan))
                {
                    unsigned char pc = panToChar(pan);
                    m_pan_lastpos[gch] = pc;                     // keep SetSurfacePan's cache coherent
                    ring = (unsigned char)(1 + ((pc * 11) >> 7));
                }
            }
            else if (GetTrackNumSends && m_enc_send < GetTrackNumSends(tr, 0)) // send level
            {
                double sv = GetTrackSendInfo_Value ? GetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL") : 0.0;
                int iv = volToInt14(sv);
                ring = (unsigned char)(1 + (iv * 10) / 16383);   // 1..11
                has_send = true;
            }
        }
        unsigned char blink = has_send ? 1 : 0;                  // only strips carrying the ridden send blink
        if (blink != m_enc_blink_last[gch])
        {
            m_enc_blink_last[gch] = blink;
            SendChannelLED(gch, 5, blink != 0);
        }
        if (!force && ring == m_enc_ring_last[gch]) return;     // live poll: only send the ring on change
        m_enc_ring_last[gch] = ring;
        m_midiouts[gch / 8]->Send(0xB0, 0x10 + (gch & 7), ring, -1);
    }

    void RefreshEncoders() { for (int i = 0; i < 24; ++i) SendEncoderRing(i); }

    // While the encoders ride sends, REAPER pushes no send-level callbacks, so poll the
    // rings and repaint only those that changed (keeps the display live without re-clicking).
    void PollSendRings()
    {
        if (m_enc_send < 0) return;
        if (m_flip) for (int i = 0; i < 24; ++i) SendFlipFader(i, false);   // FLIP: faders ride the send
        else        for (int i = 0; i < 24; ++i) SendEncoderRing(i, false); // normal: rings ride the send
    }

    // Paint one channel's scribble for the current state: a held peek (track number /
    // fader dB) wins; else send-destination name while in send mode; else the cached
    // track name. SendTrackTitle truncates to the 4-char strip.
    // Format a track's fader level as a 4-char-friendly dB string ("-inf", "+0", "-6"...).
    void FormatTrackDb(MediaTrack *tr, char *buf, int bufsz)
    {
        buf[0] = 0;
        double vol, pan;
        if (GetTrackUIVolPan && GetTrackUIVolPan(tr, &vol, &pan))
        {
            double db = VAL2DB(vol);
            if (db <= -99.0) strcpy_s(buf, bufsz, "-inf");
            else             sprintf_s(buf, bufsz, "%+.0f", db);
        }
    }

    void RefreshScribble(int gch)
    {
        if (gch < 0 || gch >= 24) return;
        MediaTrack *tr = TrackFromCh(gch);
        char buf[16] = "";
        if (tr)
        {
            if (m_auto_label_time[gch] && timeGetTime() - m_auto_label_time[gch] < 1200 && GetTrackAutomationMode)
            {                                     // momentary: just cycled the AUTO key -> show the mode
                static const char *nm[5] = { "Off ", "Read", "Touc", "Writ", "Latc" };
                int m = GetTrackAutomationMode(tr);
                strcpy_s(buf, sizeof(buf), (m >= 0 && m < 5) ? nm[m] : "Auto");
            }
            else if (m_touch_db && m_fader_touch[gch]) // riding this fader: show its dB
            {
                FormatTrackDb(tr, buf, sizeof(buf));
            }
            else if (m_scribble_peek == 1)       // hold: track number
            {
                sprintf_s(buf, sizeof(buf), "%d", CSurf_TrackToID(tr, false));
            }
            else if (m_scribble_peek == 2)       // hold: fader level in dB
            {
                FormatTrackDb(tr, buf, sizeof(buf));
            }
            else if (m_enc_send >= 0)            // send mode: destination name
            {
                if (GetTrackNumSends && m_enc_send < GetTrackNumSends(tr, 0) && GetTrackSendName)
                    GetTrackSendName(tr, m_enc_send, buf, sizeof(buf));
                else strcpy_s(buf, sizeof(buf), "-");
            }
            else                                 // normal: cached track name
            {
                int j; for (j = 0; j < 7 && m_scribble_name[gch][j]; ++j) buf[j] = m_scribble_name[gch][j];
                buf[j] = 0;
            }
        }
        SendTrackTitle(gch, buf);
    }

    void RefreshScribbles() { for (int i = 0; i < 24; ++i) RefreshScribble(i); }

    // SELECT ASSIGN readout: the encoder-assignment label is scribble strip #8 on
    // port 1 (hw-captured from Pro Tools 2026-06-17) - same zone 0x10 format as a
    // channel name, channel index 0x08. F0 00 00 66 05 00 10 08 <4 chars> F7.
    void SendSelectAssign(const char *text)
    {
        if (!m_midiouts[0]) return;
        unsigned char sx[13] = { 0xF0,0x00,0x00,0x66,0x05,0x00,0x10, 0x08, 0x20,0x20,0x20,0x20, 0xF7 };
        for (int i = 0; i < 4 && text[i]; ++i)
        {
            unsigned char c = (unsigned char)text[i];
            sx[8 + i] = (c >= 0x20 && c < 0x7F) ? c : 0x20;
        }
        char buf[sizeof(MIDI_event_t) + 13];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = 13;
        memcpy(msg->midi_message, sx, 13);
        m_midiouts[0]->SendMsg(msg, -1);
    }

    // Paint SELECT ASSIGN from the current encoder mode, using the (optionally
    // ini-overridden) labels: m_sa_pan in pan mode, m_sa_send[slot] in send mode.
    void RefreshSelectAssign()
    {
        if (!m_drive_select_assign) return;
        SendSelectAssign(m_enc_send < 0 ? m_sa_pan : m_sa_send[m_enc_send]);
    }

    // CURSOR MODE readout via zone 0x0D sw2 (hw-confirmed 2026-06-17): bit6 clear =
    // NAVIGATION, set = ZOOM. The desk's third mode, SELECT, is a transient state that
    // won't hold from a static LED (it needs continuous toggling) and, more to the point,
    // maps to no plugin behaviour -- PT's SELECT makes the arrows extend the edit
    // selection, but our arrows do scroll/zoom -- so we drive only NAVIGATION/ZOOM.
    //   ENTER arrow-mode: 0 scroll -> NAVIGATION, 1 zoom -> ZOOM.
    void SendCursorMode(int mode)
    {
        if (!m_drive_cursor_mode) return;
        SendGlobalLED(0x0D, 2, mode == 1);
    }


    // ENC ASSIGN 1/2 scribble peek. Momentary by default (overlay while held);
    // [display] peek_latch = 1 makes a press toggle the overlay on/off instead.
    void OnPeek(bool press, int mode)
    {
        if (m_peek_latch)
        {
            if (press) { m_scribble_peek = (m_scribble_peek == mode) ? 0 : mode; RefreshScribbles(); }
        }
        else { m_scribble_peek = press ? mode : 0; RefreshScribbles(); }
    }

    // light the active AUX/PAN mode button (zone 0x0B: AUX1=sw7 .. AUX5=sw3, ENC PAN=sw2)
    void SetEncModeLEDs()
    {
        for (int sw = 2; sw <= 7; ++sw) SendGlobalLED(0x0B, sw, false);
        if (m_enc_send < 0) SendGlobalLED(0x0B, 2, true);                  // PAN
        else                SendGlobalLED(0x0B, 7 - m_enc_send, true);     // AUX (slot 0->sw7)
    }

    // AUX SELECT / ENC PAN press -> switch encoder mode and repaint
    void OnEncMode(int sw)
    {
        if (sw == 2)                  m_enc_send = -1;           // ENC PAN
        else if (sw >= 3 && sw <= 7)  { if (!m_enc_sends) return; m_enc_send = 7 - sw; } // AUX 1-5 -> send 0-4
        else return;
        SetEncModeLEDs();
        if (m_flip) RefreshFlip();      // flip on: repaint faders (send) + rings (volume) for the new mode
        else        RefreshEncoders();
        RefreshScribbles();             // show send-destination names (send mode) or restore track names
        RefreshSelectAssign();          // update the SELECT ASSIGN readout (Pan / SndA-E)
    }

    // channel encoder turned while in send mode: nudge that channel's send level (1 dB/detent)
    void OnSendKnob(int gch, unsigned char data2)
    {
        MediaTrack *tr = TrackFromCh(gch);
        if (!tr || !GetTrackNumSends || !GetTrackSendInfo_Value || !SetTrackSendInfo_Value) return;
        if (m_enc_send < 0 || m_enc_send >= GetTrackNumSends(tr, 0)) return;
        int steps = data2 & 0x3F;
        if (!steps) return;
        double db = VAL2DB(GetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL"));
        db += (data2 & 0x40 ? 1.0 : -1.0) * steps;
        if (db > 12.0) db = 12.0;
        if (db < -120.0) db = -120.0;
        SetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL", DB2VAL(db));
        SendEncoderRing(gch);
    }

    // FLIP is active only when it has something to swap, i.e. while the encoders ride a
    // send. Then the motor faders ride that send and the encoders/rings ride volume.
    bool flipSends() const { return m_flip && m_enc_send >= 0; }

    // FLIPped channel encoder turned: nudge that channel's VOLUME (1 dB/detent). The ring
    // (which shows volume in flip) repaints via SetSurfaceVolume's flip branch.
    void OnVolumeKnob(int gch, unsigned char data2)
    {
        MediaTrack *tr = TrackFromCh(gch);
        if (!tr || !GetTrackUIVolPan) return;
        int steps = data2 & 0x3F;
        if (!steps) return;
        double vol, pan;
        if (!GetTrackUIVolPan(tr, &vol, &pan)) return;
        double db = VAL2DB(vol) + (data2 & 0x40 ? 1.0 : -1.0) * steps;
        if (db > 12.0) db = 12.0;
        if (db < -120.0) db = -120.0;
        CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, DB2VAL(db), false), NULL);
    }

    // FLIP + send mode: drive the motor fader from the channel's selected send level
    // (REAPER pushes no send-level callbacks, so the Run() poll calls this).
    void SendFlipFader(int gch, bool force)
    {
        if (gch < 0 || gch >= 24 || !m_midiouts[gch / 8]) return;
        MediaTrack *tr = TrackFromCh(gch);
        int iv = 0;
        if (tr && GetTrackNumSends && GetTrackSendInfo_Value && m_enc_send < GetTrackNumSends(tr, 0))
            iv = volToInt14(GetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL"));
        if (!force && iv == m_send_fader_last[gch]) return;
        m_send_fader_last[gch] = iv;
        m_midiouts[gch / 8]->Send(0xB0, gch & 7, (iv >> 7) & 0x7F, -1);
        m_midiouts[gch / 8]->Send(0xB0, 0x20 + (gch & 7), iv & 0x7F, -1);
    }

    // FADER MODE button -> toggle FLIP, drive its LED (0x43 lights AUX/MTRX = flip on,
    // 0x03 lights FADER = off), and repaint faders + rings for the new state.
    void OnFlip()
    {
        m_flip = !m_flip;
        SendGlobalLED(0x0C, 3, m_flip);
        RefreshFlip();
    }

    // Repaint every channel's fader and ring for the current flip/encoder state. In flip
    // (send mode) the fader rides the send and the ring rides volume; otherwise normal.
    void RefreshFlip()
    {
        memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));     // force fader/ring repaint
        memset(m_enc_ring_last, 0xff, sizeof(m_enc_ring_last));
        memset(m_send_fader_last, 0xff, sizeof(m_send_fader_last));
        bool flip = flipSends();
        for (int i = 0; i < 24; ++i)
        {
            MediaTrack *tr = TrackFromCh(i);
            if (tr)
            {
                double vol, pan;
                if (GetTrackUIVolPan && GetTrackUIVolPan(tr, &vol, &pan)) SetSurfaceVolume(tr, vol);
            }
            if (flip) { SendFlipFader(i, true);
                        if (m_enc_blink_last[i]) { m_enc_blink_last[i] = 0; SendChannelLED(i, 5, false); } } // no blink in flip
            else      SendEncoderRing(i);
        }
    }

    // Per-channel AUTO key, configurable via [channel] auto_button:
    //   0 unity      -> snap that fader to 0 dB
    //   1 automation -> cycle the track's REAPER automation mode (read->touch->latch->write)
    //   2 monitor    -> cycle the track's record monitor (off->on->auto)
    // The AUTO LED (lit in touch/write/latch via the poll) gives feedback in mode 1.
    void OnAutoButton(MediaTrack *tr)
    {
        if (!tr) return;
        if (m_auto_button == 0)
        {
            CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, 1.0, false), NULL); // unity gain
        }
        else if (m_auto_button == 2)
        {
            if (GetMediaTrackInfo_Value && CSurf_OnInputMonitorChange)
            {
                int cur = (int)GetMediaTrackInfo_Value(tr, "I_RECMON"); // 0=off,1=on,2=auto
                CSurf_OnInputMonitorChange(tr, (cur + 1) % 3);
            }
        }
        else if (GetTrackAutomationMode && SetTrackAutomationMode)
        {
            static const int order[5] = { AUTO_MODE_TRIM, AUTO_MODE_READ, AUTO_MODE_TOUCH, AUTO_MODE_LATCH, AUTO_MODE_WRITE };
            int cur = GetTrackAutomationMode(tr), next = AUTO_MODE_TRIM;
            for (int i = 0; i < 5; ++i) if (order[i] == cur) { next = order[(i + 1) % 5]; break; }
            SetTrackAutomationMode(tr, next);
        }
    }

    // val: 0x40|sw = press, 0x00|sw = release
    void OnSwitch(int port, int zone, unsigned char val)
    {
        if (zone < 0) return;
        bool press = !!(val & 0x40);
        int sw = val & 0x0F;

        if (zone < 8)                            // channel strip zones: zone = channel on this port
        {
            int gch = port * 8 + zone;
            if (sw == 0)                         // fader touch / release
            {
                m_fader_touch[gch] = press ? 1 : 0;
                if (m_double_touch)              // double-tap a fader -> snap it to 0 dB (REAPER double-click)
                {
                    if (press)
                    {
                        // detect the 2nd tap, but defer the snap to RELEASE: applying 0 dB while the
                        // fader is still held loses to its position echo (hw-confirmed), so it won't stick
                        DWORD tnow = timeGetTime();
                        if (tnow - m_fader_touchtime[gch] < 500) m_snap_pending[gch] = 1;
                        m_fader_touchtime[gch] = tnow;
                    }
                    else if (m_snap_pending[gch]) // release of the 2nd tap: finger off -> the snap sticks
                    {
                        m_snap_pending[gch] = 0;
                        MediaTrack *trd = TrackFromCh(gch);
                        if (trd) CSurf_SetSurfaceVolume(trd, CSurf_OnVolumeChange(trd, 1.0, false), NULL); // unity
                    }
                }
                if (m_touch_db) RefreshScribble(gch); // swap strip to dB on touch, name on release
            }
            else if (sw == 1)                    // SELECT: tracked on press AND release (hold-to-multi-select)
            {
                OnSelectButton(gch, press);
            }
            else if (press)
            {
                MediaTrack *tr = TrackFromCh(gch);
                if (!tr) return;
                switch (sw)
                {
                    case 2: CSurf_SetSurfaceMute(tr, CSurf_OnMuteChange(tr, -1), NULL); break; // MUTE
                    case 3: CSurf_SetSurfaceSolo(tr, CSurf_OnSoloChange(tr, -1), NULL); break; // SOLO
                    case 4: // AUTO key: configurable ([channel] auto_button)
                        OnAutoButton(tr);
                        if (m_auto_button == 1 && gch < 24)  // automation mode: flash the new mode on the strip
                        { m_auto_label_time[gch] = timeGetTime(); RefreshScribble(gch); }
                        break;
                    case 5: // V-pot press: flip -> reset volume to 0 dB; send mode -> reset send; else center pan
                        if (flipSends())  // FLIP: the encoder rides volume, so reset it to unity
                        {
                            CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, 1.0, false), NULL);
                        }
                        else if (m_enc_send >= 0 && GetTrackNumSends && SetTrackSendInfo_Value &&
                            m_enc_send < GetTrackNumSends(tr, 0))
                        {
                            SetTrackSendInfo_Value(tr, 0, m_enc_send, "D_VOL", DB2VAL(0.0));
                            SendEncoderRing(gch);
                        }
                        else
                        {
                            CSurf_SetSurfacePan(tr, CSurf_OnPanChange(tr, 0.0, false), NULL);
                            m_pan_lasttouch[gch] = timeGetTime();
                        }
                        break;
                    case 7: CSurf_OnRecArmChange(tr, -1); break;                               // REC/RDY
                }
            }
        }
        else if (zone == 0x08 && press && port == 0) // BACK/FORWARD (hw-verified) + UDK 4/5/12/13/15/16
        {
            switch (sw)                              // full zone hw-verified 2026-06-16 in-order capture
            {
                case 2: SendMessage(g_hwnd, WM_COMMAND, IDC_EDIT_UNDO, 0); break; // BACK -> undo
                case 6: SendMessage(g_hwnd, WM_COMMAND, IDC_EDIT_REDO, 0); break; // FORWARD -> redo
                case 1: DispatchUDK(3);  break;  // UDK 4
                case 5: DispatchUDK(4);  break;  // UDK 5
                case 0: DispatchUDK(11); break;  // UDK 12
                case 4: DispatchUDK(12); break;  // UDK 13
                case 3: DispatchUDK(14); break;  // UDK 15
                case 7: if (!DispatchUDK(15)) DumpFXParams(); break; // UDK 16; unmapped -> dump FX params
            }
        }
        else if (zone == 0x09 && press && port == 0) // UDK 1 (sw2), UDK 9 (sw0; sw1 fires alongside - ignore)
        {
            if (sw == 2)      DispatchUDK(0); // UDK 1
            else if (sw == 0) DispatchUDK(8); // UDK 9
            else if (sw == 5)                 // EFFECTS/PLUG-INS DISPLAY (no LED): toggle ONLY whether a
            {                                 // knob-touch auto-floats the FX window. F4/PARAM and the knobs
                m_window_on_knob = !m_window_on_knob;   // still show/hide it; this never moves the window itself.
                for (int c = 0; c < 8; ++c) SendDisplayCell(c, "");   // clear all 8 (2 rows x 4 cols) for the flash
                SendDisplayCell(1, "VST plugin");                     // centred in the middle two columns:
                SendDisplayCell(2, " window");                        //   top row: "VST plugin window" (cells abut,
                                                                      //   and "VST plugin" fills cell 1, so lead-space)
                SendDisplayCell(5, "auto-show");                      //   btm row  (cols 2-3): auto-show ON/OFF
                SendDisplayCell(6, m_window_on_knob ? "ON" : "OFF");
                m_winflash_time = timeGetTime();
            }
        }
        else if (zone == 0x0A && press)          // BANK/CH arrows; also UDK 3/4/10/11
        {
            // These four buttons double as UDK 3/4/10/11. An ini [udk] action takes
            // priority; with no ini entry they keep factory bank/channel navigation.
            static const int udk_idx[4] = { 9, 1, 10, 2 }; // sw0=UDK10 sw1=UDK2 sw2=UDK11 sw3=UDK3
            if (sw > 3 || !DispatchUDK(udk_idx[sw]))
            {
                int amt = 0;
                switch (sw)
                {
                    case 0: amt = -1; break;     // channel left
                    case 1: amt = -24; break;    // bank left
                    case 2: amt = 1; break;      // channel right
                    case 3: amt = 24; break;     // bank right
                }
                if (amt) AdjustBankOffset(amt);
            }
        }
        else if (zone == 0x0B && port == 0)      // ENCODER MODE / AUX SELECT + ENC ASSIGN peek modifiers
        {
            if (sw == 1 && m_peek_number)      OnPeek(press, 1); // ENC ASSIGN1: track number
            else if (sw == 0 && m_peek_db)     OnPeek(press, 2); // ENC ASSIGN2: fader dB
            else if (press && sw >= 2)         OnEncMode(sw);    // PAN (sw2) / AUX 1-5 (sw3-7)
        }
        else if (zone == 0x0E && !press && (sw == 1 || sw == 2))
        {
            m_held_transport = -1;               // REW/FF released: cancel auto-repeat
        }
        else if (zone == 0x0E && press)          // transport (hw-verified: sw1=REW sw2=FF sw3=STOP sw4=PLAY sw5=REC)
        {
            switch (sw)
            {
                case 1: CSurf_OnRew(1); m_held_transport = 1; m_transport_held_since = timeGetTime(); break;
                case 2: CSurf_OnFwd(1); m_held_transport = 2; m_transport_held_since = timeGetTime(); break;
                case 3: CSurf_OnStop(); break;
                case 4: CSurf_OnPlay(); break;
                case 5: CSurf_OnRecord(); break;
            }
        }
        else if (zone == 0x0F && press)  // hw-verified 2026-06-15: sw0=RTZ sw1=END sw2=ONLINE sw3=LOOP sw4=QUICK PUNCH
        {                                // SET/REHEARSAL/MTR/MASTER do not transmit HUI - DM2000 internal only
            switch (sw)
            {
                case 0: if (m_la_rtz)    SendMessage(g_hwnd, WM_COMMAND, m_la_rtz,    0); else CSurf_GoStart(); break; // RTZ
                case 1: if (m_la_end)    SendMessage(g_hwnd, WM_COMMAND, m_la_end,    0); else CSurf_GoEnd();   break; // END
                case 2: if (m_la_online) SendMessage(g_hwnd, WM_COMMAND, m_la_online, 0); break;                       // ONLINE
                case 3: if (m_la_loop)   SendMessage(g_hwnd, WM_COMMAND, m_la_loop,   0); break;                       // LOOP
                case 4: if (m_la_qpunch) SendMessage(g_hwnd, WM_COMMAND, m_la_qpunch, 0); break;                       // QUICK PUNCH
            }
        }
        else if (zone == 0x10 && press)  // hw-verified 2026-06-15: sw0=AUDITION sw1=PRE sw2=IN sw3=OUT sw4=POST
        {
            switch (sw)
            {
                case 0: if (m_la_audition) SendMessage(g_hwnd, WM_COMMAND, m_la_audition, 0); break; // AUDITION
                case 1: if (m_la_pre)      SendMessage(g_hwnd, WM_COMMAND, m_la_pre,      0); break; // PRE
                case 2: if (m_la_in)   SendMessage(g_hwnd, WM_COMMAND, m_la_in,   0); break; // IN
                case 3: if (m_la_out)  SendMessage(g_hwnd, WM_COMMAND, m_la_out,  0); break; // OUT
                case 4: if (m_la_post) SendMessage(g_hwnd, WM_COMMAND, m_la_post, 0); break; // POST
            }
        }
        else if (zone == 0x0D && !press && (sw == 4 || sw == 0 || sw == 1 || sw == 3))
        {
            m_held_arrow = -1;                   // arrow released: cancel auto-repeat
        }
        else if (zone == 0x0D && press)          // cursor cluster + wheel-mode keys (hardware-verified)
        {
            switch (sw)
            {
                // arrows: fire immediately and arm auto-repeat in Run()
                case 4: CSurf_OnArrow(0, m_arrow_mode == 1); m_held_arrow = 0; m_arrow_held_since = timeGetTime(); break; // up
                case 0: CSurf_OnArrow(1, m_arrow_mode == 1); m_held_arrow = 1; m_arrow_held_since = timeGetTime(); break; // down
                case 1: // left: scroll (NAVIGATION) or zoom (ZOOM)
                    CSurf_OnArrow(2, m_arrow_mode == 1);
                    m_held_arrow = 2; m_arrow_held_since = timeGetTime();
                    break;
                case 3: // right: scroll (NAVIGATION) or zoom (ZOOM)
                    CSurf_OnArrow(3, m_arrow_mode == 1);
                    m_held_arrow = 3; m_arrow_held_since = timeGetTime();
                    break;
                case 2:                                        // INC -> next marker
                    SendMessage(g_hwnd, WM_COMMAND, ID_MARKER_NEXT, 0);
                    break;
                case 5:                                        // SCRUB: wheel scrubs audio
                    m_wheel_mode = (m_wheel_mode == 1) ? 0 : 1;
                    SendGlobalLED(0x0D, 5, m_wheel_mode == 1);
                    SendGlobalLED(0x0D, 6, false);
                    break;
                case 6:                                        // SHUTTLE: wheel moves coarse
                    m_wheel_mode = (m_wheel_mode == 2) ? 0 : 2;
                    SendGlobalLED(0x0D, 6, m_wheel_mode == 2);
                    SendGlobalLED(0x0D, 5, false);
                    break;
            }
        }
        else if (zone == 0x13 && press && sw != 5) // LOCATE MEMORY 1-6 (hw-verified; sw5 is companion event)
        {
            // non-sequential sw: LM1=sw1 LM2=sw3 LM3=sw6 LM4=sw2 LM5=sw4 LM6=sw7
            static const int lm_idx[] = { -1, 0, 3, 1, 4, -1, 2, 5 }; // index by sw -> m_la_lm index
            if (sw < 8 && lm_idx[sw] >= 0 && m_la_lm[lm_idx[sw]])
                SendMessage(g_hwnd, WM_COMMAND, m_la_lm[lm_idx[sw]], 0);
        }
        else if (zone == 0x15 && press && (sw == 0 || sw == 1)) // LM7/LM8 (hw-captured 2026-06-15)
        {
            if (m_la_lm[6 + sw])
                SendMessage(g_hwnd, WM_COMMAND, m_la_lm[6 + sw], 0);
        }
        else if (zone == 0x1B && sw == 7 && press) // DEC -> previous marker
        {
            SendMessage(g_hwnd, WM_COMMAND, ID_MARKER_PREV, 0);
        }
        else if (zone == 0x14 && sw == 0 && press) // ENTER: toggle cursor arrows NAVIGATION (scroll) <-> ZOOM
        {
            m_arrow_mode = (m_arrow_mode + 1) % 2;
            SendCursorMode(m_arrow_mode); // update the CURSOR MODE readout (NAVIGATION/ZOOM)
        }
        else if (zone == 0x19 && press && port == 0) // UDK 6/7/8/14 (broadcast on 3 ports, deduped via port 0)
        {
            switch (sw)
            {
                case 5: DispatchUDK(5);  break; // UDK 6
                case 3: DispatchUDK(6);  break; // UDK 7
                case 4: DispatchUDK(7);  break; // UDK 8
                case 1: DispatchUDK(13); break; // UDK 14
            }
        }
        else if (zone == 0x0C && sw == 2 && press && port == 0) // AUTOMIX ENABLE = Pro Tools SUSPEND (configurable)
        {
            if (m_automix_suspend) SendMessage(g_hwnd, WM_COMMAND, m_automix_suspend, 0);
        }
        else if (zone == 0x0C && sw == 3 && press && port == 0) // FADER MODE = FLIP (faders <-> send encoders)
        {
            if (m_flip_enabled) OnFlip();
        }
        else if (zone == 0x18 && press && port == 0) // AUTOMIX automation-mode buttons (hw-verified 2026-06-17)
        {
            // Each sets the SELECTED tracks' automation mode (select all to apply to all).
            int mode = -1;
            switch (sw)
            {
                case 0: mode = AUTO_MODE_TRIM;  break; // RELATIVE  -> Trim
                case 1: mode = AUTO_MODE_LATCH; break; // AUTO-REC  -> Latch
                case 2: mode = AUTO_MODE_READ;  break; // RETURN    -> Read
                case 3: mode = AUTO_MODE_TRIM;  break; // TOUCH SENSE (Off) -> Trim (REAPER has no per-track Off)
                case 4: mode = AUTO_MODE_WRITE; break; // REC        -> Write
                case 5: mode = AUTO_MODE_TOUCH; break; // ABORT/UNDO -> Touch
            }
            if (mode >= 0)
            {
                if (SetAutomationMode) SetAutomationMode(mode, true); // only selected tracks
                m_auto_mode = mode;
                SendAutomixLEDs(mode);
            }
        }
        else if (zone == 0x17 && press && port == 0) // AUTOMIX-OVERWRITE param-arm row -> configurable [automix] actions
        {
            int act = 0;
            switch (sw)
            {
                case 0: act = m_ow_eq;    break; // EQ     (Pro Tools Plug-in)
                case 1: act = m_ow_pan;   break; // PAN
                case 2: act = m_ow_fader; break; // FADER  (Pro Tools Volume)
                case 3: act = m_ow_auxon; break; // AUX ON (Pro Tools Send mute)
                case 4: act = m_ow_aux;   break; // AUX    (Pro Tools Send)
                case 5: act = m_ow_on;    break; // ON     (Pro Tools Mute)
            }
            if (act) SendMessage(g_hwnd, WM_COMMAND, act, 0);
        }
        else if (zone == 0x1C && press && port == 0) // EFFECTS/PLUG-INS section: generic FX parameter editor
        {
            OnFXSwitch(sw);
        }
    }

    void AdjustBankOffset(int amt)
    {
        int maxoffs = CSurf_NumTracks(false) - 1;
        if (maxoffs < 0) maxoffs = 0;

        int offs = m_bank_offset + amt;
        if (offs < 0) offs = 0;
        else if (offs > maxoffs) offs = maxoffs;
        if (offs == m_bank_offset) return;

        m_bank_offset = offs;
        m_held_arrow = -1;
        memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
        memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
        memset(m_meter_lastlvl, 0xff, sizeof(m_meter_lastlvl));
        memset(m_meter_hist, 0, sizeof(m_meter_hist));
        memset(m_fader_touch, 0, sizeof(m_fader_touch));
        memset(m_fader_touchtime, 0, sizeof(m_fader_touchtime)); // physical-strip cache: stale tap time must not snap the new track to 0 dB
        memset(m_snap_pending, 0, sizeof(m_snap_pending));       // and no armed snap should carry across a bank/track change
        TrackList_UpdateAllExternalSurfaces(); // repaint faders/LEDs for the new bank
        SetTrackListChange();                  // blank scribbles on channels past the last track
        if (m_enc_send >= 0) RefreshEncoders(); // send-level rings aren't driven by SetSurfacePan
        if (m_enc_send >= 0 || m_scribble_peek) RefreshScribbles(); // repaint override scribbles for the new bank
    }

    // Fire a User Defined Key's configured action. idx 0-15 = UDK 1-16.
    // Returns true if an action was dispatched (used for the zone 0x0A nav fallback).
    bool DispatchUDK(int idx)
    {
        if (idx < 0 || idx >= 16 || !m_udk[idx] || !Main_OnCommand) return false;
        Main_OnCommand(m_udk[idx], 0);
        return true;
    }

    // Decode a relative 7-bit signed encoder byte: 0x01..0x3F = +n (CW),
    // 0x40..0x7F = -(0x80-n) (CCW, so 0x7F = -1). 0 = no movement.
    static int relStep(unsigned char v) { return (v < 0x40) ? (int)v : ((int)v - 0x80); }

    // Nudge an FX parameter by `steps` * `scale` of its normalized range, then clamp.
    void NudgeFXParam(MediaTrack *tr, int fx, int param, int steps, double scale = 0.01)
    {
        if (!TrackFX_GetParam || !TrackFX_SetParam) return;
        double mn = 0.0, mx = 1.0;
        double cur = TrackFX_GetParam(tr, fx, param, &mn, &mx);
        double range = mx - mn;
        if (range == 0.0) return;            // zero-range param: nothing to nudge
        double norm = (cur - mn) / range + steps * scale;
        if (norm < 0.0) norm = 0.0; else if (norm > 1.0) norm = 1.0;
        TrackFX_SetParam(tr, fx, param, mn + norm * range);
    }

    // Set an FX parameter to an absolute normalized 0..1 position (joystick).
    void SetFXParamNorm(MediaTrack *tr, int fx, int param, double norm)
    {
        if (!TrackFX_GetParam || !TrackFX_SetParam) return;
        if (norm < 0.0) norm = 0.0; else if (norm > 1.0) norm = 1.0;
        double mn = 0.0, mx = 1.0;
        TrackFX_GetParam(tr, fx, param, &mn, &mx);
        TrackFX_SetParam(tr, fx, param, mn + norm * (mx - mn));
    }

    // Shared accumulator cache for relative FX-param knobs (surround dynamics + FX-editor SEL),
    // keyed by the *target* (track, fx, param). Any control writing a given param shares one running
    // value - so THRESHOLD and SEL4 both driving the surround X stay in lockstep. We write absolutely
    // and never read the value back mid-turn: a smoothed param hands back a laggy/mirrored value that
    // read-modify-write would fight. Round-robin eviction; a re-seed is one settled (accurate) read.
    int AccSlot(MediaTrack *tr, int fx, int param)
    {
        for (int i = 0; i < 16; i++)
            if (m_acc_tr[i] == tr && m_acc_fx[i] == fx && m_acc_param[i] == param) return i;
        int s = m_acc_next; m_acc_next = (m_acc_next + 1) & 15;   // evict oldest
        m_acc_tr[s] = tr; m_acc_fx[s] = fx; m_acc_param[s] = param;
        double mn = 0.0, mx = 1.0;
        double cur = TrackFX_GetParam ? TrackFX_GetParam(tr, fx, param, &mn, &mx) : 0.0;
        double range = mx - mn;
        m_acc_val[s] = (range != 0.0) ? (cur - mn) / range : 0.0; // seed once from the plugin
        return s;
    }
    // Read-only: the cached normalized value if we own this param, else -1 (no allocation). The FX
    // display uses it so it shows the value we wrote, not the plugin's mirrored/laggy read-back.
    double AccPeekCached(MediaTrack *tr, int fx, int param)
    {
        for (int i = 0; i < 16; i++)
            if (m_acc_tr[i] == tr && m_acc_fx[i] == fx && m_acc_param[i] == param) return m_acc_val[i];
        return -1.0;
    }
    // Relative nudge: cap the desk's speed acceleration, accumulate, write absolutely.
    void AccelNudge(MediaTrack *tr, int fx, int param, int delta, double scale)
    {
        if (delta > 1) delta = 1; else if (delta < -1) delta = -1;
        if (!delta) return;
        int s = AccSlot(tr, fx, param);
        m_acc_val[s] += delta * scale;
        if (m_acc_val[s] < 0.0) m_acc_val[s] = 0.0; else if (m_acc_val[s] > 1.0) m_acc_val[s] = 1.0;
        SetFXParamNorm(tr, fx, param, m_acc_val[s]);
    }
    // Absolute write (joystick, or a programmatic reset) that must keep the shared value in sync.
    void AccelSet(MediaTrack *tr, int fx, int param, double norm)
    {
        if (norm < 0.0) norm = 0.0; else if (norm > 1.0) norm = 1.0;
        SetFXParamNorm(tr, fx, param, norm);
        m_acc_val[AccSlot(tr, fx, param)] = norm;
    }

    // Reset an FX parameter to its neutral/default value. REAPER has no per-plugin
    // "default" accessor; TrackFX_GetParamEx's midval is the closest (the parameter's
    // neutral point - e.g. center for pan/gain). Falls back to the minimum if absent.
    void ResetFXParamDefault(MediaTrack *tr, int fx, int param)
    {
        if (!TrackFX_SetParam) return;
        double norm = 0.0;
        if (TrackFX_GetParamEx)
        {
            double mn = 0.0, mx = 1.0, mid = 0.0;
            TrackFX_GetParamEx(tr, fx, param, &mn, &mx, &mid);
            TrackFX_SetParam(tr, fx, param, mid);
            double range = mx - mn;
            norm = (range != 0.0) ? (mid - mn) / range : 0.0;
        }
        else SetFXParamNorm(tr, fx, param, 0.0); // older REAPER: fall back to minimum
        m_acc_val[AccSlot(tr, fx, param)] = norm; // keep the shared accumulator in sync with the reset
    }

    // Log current FX/param state to the REAPER console (temporary mapping aid).
    // Only emits when the (fx, param) target changes, so a knob sweep doesn't flood.
    void LogFXParam(const char *tag, MediaTrack *tr, int fx, int param)
    {
        if (!m_console_log || !ShowConsoleMsg || !tr) return;
        int key = fx * 1000 + param;
        if (key == m_fx_loglast) return;
        m_fx_loglast = key;
        char fxname[128] = "", pname[128] = "";
        if (TrackFX_GetFXName)    TrackFX_GetFXName(tr, fx, fxname, sizeof(fxname));
        if (TrackFX_GetParamName) TrackFX_GetParamName(tr, fx, param, pname, sizeof(pname));
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, fx) : -1;
        char msg[360];
        sprintf_s(msg, sizeof(msg), "[DM2000 %s] '%s' (%d params): param %d = '%s'\n",
                  tag, fxname, np, param, pname);
        ShowConsoleMsg(msg);
    }

    // Dump every param name of the selected track's first FX to the console - a
    // configuration aid for finding the right [surround] indices for any panner.
    void DumpFXParams()
    {
        if (!ShowConsoleMsg) return;
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        if (!tr || !TrackFX_GetCount || TrackFX_GetCount(tr) < 1)
        {
            ShowConsoleMsg("[DM2000 dump] selected track has no FX\n");
            return;
        }
        int slot = m_fx_slot;                              // dump the slot the FX editor is on, not always FX 0
        if (slot < 0 || slot >= TrackFX_GetCount(tr)) slot = 0;
        char fxname[128] = "";
        if (TrackFX_GetFXName) TrackFX_GetFXName(tr, slot, fxname, sizeof(fxname));
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, slot) : 0;
        char hdr[200];
        sprintf_s(hdr, sizeof(hdr), "[DM2000 dump] FX %d '%s' - %d params:\n", slot, fxname, np);
        ShowConsoleMsg(hdr);
        for (int p = 0; p < np; ++p)
        {
            char pname[128] = "";
            if (TrackFX_GetParamName) TrackFX_GetParamName(tr, slot, p, pname, sizeof(pname));
            char line[180];
            sprintf_s(line, sizeof(line), "  %d = %s\n", p, pname);
            ShowConsoleMsg(line);
        }
    }

    // MCS PANNER (port 4) surround control: dynamics knobs (relative) + joystick
    // (absolute) drive the configured [surround] plugin on the selected track.
    // Returns true if the message was consumed.
    bool OnPannerCC(unsigned char data1, unsigned char data2)
    {
        if (data1 == 0x00) return true;   // routing button companion (BE 00 N) - ignore
        if (data1 == 0x01)                // routing buttons (BE 01 N): object 1-8 / Direct bank
        {
            OnRoutingButton(data2);
            return true;
        }

        // map this control to a configured surround param + how to apply it
        int param = -1;
        bool absolute = false;            // joystick = absolute position; knobs = relative
        bool invert = false;              // joystick Y reads inverted vs the plugin's axis
        switch (data1)
        {
            // DM2000 dynamics knobs send CCs OUT of physical order (captured on port 4):
            // THRESHOLD=0x10, ATTACK=0x11, DECAY=0x12, RANGE=0x13, HOLD=0x14. Map each to its axis.
            case 0x10: param = m_surround_param[0]; break;                 // THRESHOLD -> X
            case 0x13: param = m_surround_param[1]; break;                 // RANGE     -> Y
            case 0x11: param = m_surround_param[2]; break;                 // ATTACK    -> Z
            case 0x12: param = m_surround_param[3]; break;                 // DECAY     -> spread
            case 0x14: param = m_surround_param[4]; break;                 // HOLD      -> gain
            case 0x02: param = m_surround_param[0]; absolute = true; break; // joystick X -> X
            case 0x03: param = m_surround_param[1]; absolute = true; invert = true; break; // joystick Y -> Y
            default: return false;                                          // not a surround control
        }

        if (!m_surround_plugin[0]) return true; // no [surround] plugin configured -> disabled
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        if (!tr || !TrackFX_GetByName) return true;
        int fx = TrackFX_GetByName(tr, m_surround_plugin, false);
        if (fx < 0) return true;          // not on the track -> do nothing (never auto-insert)

        param += m_surround_obj * m_surround_stride; // shift to the selected object
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, fx) : 0;
        if (param < 0 || param >= np) return true;

        // Apply via the shared accumulator (keyed by track/fx/param): a SEL knob or the joystick
        // touching this same param stays in sync, and a smoothed/mirrored read-back can't oscillate.
        if (absolute)
            AccelSet(tr, fx, param, invert ? 1.0 - data2 / 127.0 : data2 / 127.0); // joystick 0..127
        else
            AccelNudge(tr, fx, param, relStep(data2), 0.01);                         // dynamics knob (relative)
        // auto-float the surround plug-in on use (same [fx] window_on_knob behaviour as the FX-editor
        // knobs) - float the surround FX itself, not the FX-editor's current slot; only if not already up
        if (m_window_on_knob && TrackFX_Show &&
            !(TrackFX_GetFloatingWindow && TrackFX_GetFloatingWindow(tr, fx)))
            TrackFX_Show(tr, fx, 3);
        LogFXParam("surround", tr, fx, param);
        // If the FX editor is showing this same plugin, live-update its readout - throttled ~30 Hz since
        // the dynamics knobs/joystick flood messages. RefreshFXDisplay reads our accumulator, so it shows
        // the value we just wrote, not the plugin's mirrored read-back.
        if (m_fx_slot == fx)
        {
            DWORD now = timeGetTime();
            if (now - m_fxdisp_throttle >= 33) { m_fxdisp_throttle = now; RefreshFXDisplay(); }
        }
        return true;
    }

    // ROUTING button (BE 01 N): buttons 1-8 pick an object within the current bank of 8;
    // Direct (N=8) shifts the bank (single press up, double press down). The console sends
    // N in a 2-column order, so off[] maps the wire value back to object-within-bank.
    void OnRoutingButton(unsigned char n)
    {
        if (!m_surround_plugin[0]) return;          // object select only when [surround] configured
        if (n == 8) { OnSurroundDirect(); return; } // Direct
        if (n > 7) return;
        static const int off[8] = { 0, 2, 4, 6, 1, 3, 5, 7 }; // wire N -> object-within-bank
        m_direct_pending = false;                   // a routing press cancels a pending Direct
        SetSurroundObject((m_surround_obj / 8) * 8 + off[n]);
    }

    // Direct button: one press = bank up (+8), confirmed after 400ms; a second press
    // within that window = bank down (-8). Bidirectional, no wrap (good for 100+ objects).
    void OnSurroundDirect()
    {
        DWORD now = timeGetTime();
        if (m_direct_pending && now - m_direct_time < 400)
        {
            m_direct_pending = false;
            SetSurroundObject(m_surround_obj - 8);  // double press -> bank down
        }
        else { m_direct_pending = true; m_direct_time = now; }
    }

    // Light/clear a routing-button LED on port 4. pos = object-within-bank (0-7);
    // the console sends/expects N in a 2-column order, so wire[] maps pos -> N.
    // `BE 00 N` lights, `BE 01 N` clears (hw-captured 2026-06-16).
    void SendRoutingLED(int pos, bool on)
    {
        if (pos < 0 || pos > 7 || !m_midiouts[3]) return;
        static const int wire[8] = { 0, 4, 1, 5, 2, 6, 3, 7 };
        m_midiouts[3]->Send(0xBE, on ? 0x00 : 0x01, (unsigned char)wire[pos], -1);
    }

    // Light the selected object's routing button and clear the other 7 in the bank
    // (the console powers the routing LEDs on by default, so the rest must be cleared).
    // `active` = the selected track has the surround plugin; when false, all 8 clear so a
    // stale highlight doesn't linger on a track with no panner. Driven by the Run() poll
    // below (selection-change events from the console aren't reliable).
    void RefreshRoutingLEDs(bool active)
    {
        int sel = m_surround_obj % 8;
        for (int p = 0; p < 8; ++p) SendRoutingLED(p, active && p == sel);
    }

    // Poll the selection + object and refresh the routing LEDs only when the displayed
    // state changes. Authoritative - covers track switches, plugin add/remove, and the
    // initial/default object that SetSurroundObject's early-return would otherwise miss.
    void PollRoutingLEDs()
    {
        if (!m_surround_plugin[0]) return; // surround not configured -> never touch routing LEDs
        MediaTrack *sel = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        bool active = sel && TrackFX_GetByName && TrackFX_GetByName(sel, m_surround_plugin, false) >= 0;
        int state = active ? (1 + (m_surround_obj % 8)) : 0; // 0 = none lit, 1-8 = which button
        if (state != m_routing_led_state) { m_routing_led_state = state; RefreshRoutingLEDs(active); }
    }

    // Effective object count for the top clamp: the [surround] `objects` override if set,
    // otherwise auto-detected at runtime from the plugin's `in N` input params on the
    // selected track (0 = couldn't detect -> no top clamp).
    int SurroundObjectCount()
    {
        if (m_surround_objects > 0) return m_surround_objects;     // explicit ini override
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        if (!tr || !m_surround_plugin[0] || !TrackFX_GetByName ||
            !TrackFX_GetNumParams || !TrackFX_GetParamName) return 0;
        int fx = TrackFX_GetByName(tr, m_surround_plugin, false);
        if (fx < 0) return 0;
        int np = TrackFX_GetNumParams(tr, fx), maxin = 0;
        for (int p = 0; p < np; ++p)
        {
            char nm[64] = "";
            if (TrackFX_GetParamName(tr, fx, p, nm, sizeof(nm)) &&
                (nm[0] == 'i' || nm[0] == 'I') && (nm[1] == 'n' || nm[1] == 'N') && nm[2] == ' ')
            {
                int n = atoi(nm + 3);                             // "in 12 X" -> 12
                if (n > maxin) maxin = n;
            }
        }
        return maxin; // 0 if no "in N" params (non-standard panner) -> caller leaves unclamped
    }

    // Clamp + apply a new object selection (0-based); the Run() poll updates the LEDs.
    void SetSurroundObject(int obj)
    {
        if (obj < 0) obj = 0;
        int maxobj = SurroundObjectCount();
        if (maxobj > 0 && obj > maxobj - 1) obj = maxobj - 1;
        if (obj == m_surround_obj) return;
        m_surround_obj = obj;
        if (m_console_log && ShowConsoleMsg)
        {
            char msg[120];
            sprintf_s(msg, sizeof(msg), "[DM2000 surround] object %d (param base +%d)\n",
                      m_surround_obj + 1, m_surround_obj * m_surround_stride);
            ShowConsoleMsg(msg);
        }
    }

    // Parameter knob 1-4 (port 1, CC 0x48-0x4B) -> current FX slot param on this page.
    // Always live: edits the selected track's current FX slot (no mode to enter).
    // REMOTE "INSERT ASSIGN/EDIT" text display (HUI zone 0x12): 8 cells x 10 chars -
    // cells 0-3 = top line above knobs 1-4, cells 4-7 = bottom line (hw-mapped 2026-06-16).
    // SysEx is 7-bit, so only printable ASCII is sent; the field is space-padded to 10.
    void SendDisplayCell(int cell, const char *text)
    {
        if (!m_midiouts[0] || cell < 0 || cell > 7) return;
        unsigned char sx[19] = { 0xF0,0x00,0x00,0x66,0x05,0x00,0x12,(unsigned char)cell,
                                 0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20, 0xF7 };
        for (int i = 0; i < 10 && text[i]; ++i)
        {
            unsigned char c = (unsigned char)text[i];
            sx[8 + i] = (c >= 0x20 && c < 0x7F) ? c : 0x20;
        }
        char buf[sizeof(MIDI_event_t) + 19];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = 19;
        memcpy(msg->midi_message, sx, 19);
        m_midiouts[0]->SendMsg(msg, -1);
    }

    // Paint the FX editor onto that display: top line = the 4 current-page param NAMES,
    // bottom line = their formatted VALUES, one column per param knob. Columns past the
    // last parameter are blanked. Driven from the FX-editor knob/page/slot handlers.
    void RefreshFXDisplay()
    {
        if (!m_splash_done) return;          // hold the FX view until the startup splash finishes
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        int n = (tr && TrackFX_GetCount) ? TrackFX_GetCount(tr) : 0;
        if (m_fx_slot >= n) m_fx_slot = n > 0 ? n - 1 : 0; // keep the cached slot valid after the FX count shrinks
        bool slotValid = tr && m_fx_slot >= 0 && m_fx_slot < n;
        int np = (slotValid && TrackFX_GetNumParams) ? TrackFX_GetNumParams(tr, m_fx_slot) : 0;
        for (int k = 0; k < 4; ++k)
        {
            char name[32] = "", val[32] = "";
            int param = m_fx_page * 4 + k;
            bool live = slotValid && param < np;
            unsigned char ringv = 0;           // EFFECTS SEL ring (CC 0x18+k): 0 = off when no live param
            if (live)
            {
                if (TrackFX_GetParamName) TrackFX_GetParamName(tr, m_fx_slot, param, name, sizeof(name));
                double pmin = 0, pmax = 1;
                double vread = TrackFX_GetParam ? TrackFX_GetParam(tr, m_fx_slot, param, &pmin, &pmax) : 0.0;
                // Prefer our own accumulated value when we drive this param (the plugin's read-back can be
                // mirrored/laggy, e.g. ReaSurroundPan X); fall back to the plugin's value otherwise.
                double cached = AccPeekCached(tr, m_fx_slot, param);
                double norm = (cached >= 0.0) ? cached : ((pmax > pmin) ? (vread - pmin) / (pmax - pmin) : 0.0);
                double v = (cached >= 0.0) ? (pmin + norm * (pmax - pmin)) : vread;
                if (TrackFX_FormatParamValue) TrackFX_FormatParamValue(tr, m_fx_slot, param, v, val, sizeof(val));
                if (norm < 0) norm = 0; else if (norm > 1) norm = 1;
                // TODO(smart-ring): per-param mode - boostcut (0x10) for bipolar params (normalized
                // neutral ~0.5 via TrackFX_GetParamEx), else fill. For now fill suits most params.
                ringv = (unsigned char)(0x20 | (1 + (int)(norm * 10 + 0.5)));   // fill mode, position 1..11
            }
            SendDisplayCell(k, name);          // top line: parameter name
            SendDisplayCell(k + 4, val);       // bottom line: formatted value
            SendGlobalLED(0x1C, 2 + k, live);  // knob-SEL box (sw2-5): lit when that knob drives a real param
            if (m_midiouts[0]) m_midiouts[0]->Send(0xB0, 0x18 + k, ringv, -1);  // EFFECTS SEL parameter ring
        }
        // EFFECTS/PLUG-INS indicator boxes (zone 0x1C, hw-captured): PARAM lit while a slot
        // is shown; BYPASS lit when that slot is bypassed (the state the screen used to hide).
        SendGlobalLED(0x1C, 0, slotValid);                                              // PARAM (sw0)
        SendGlobalLED(0x1C, 6, slotValid && TrackFX_GetEnabled && !TrackFX_GetEnabled(tr, m_fx_slot)); // BYPASS (sw6)
    }

    void OnParamKnob(int knob, unsigned char data2)
    {
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        if (!tr || !TrackFX_GetCount) return;
        int n = TrackFX_GetCount(tr);
        if (m_fx_slot < 0 || m_fx_slot >= n) return;

        int delta = -relStep(data2); // hw: these knobs read reversed vs the page encoder; flip to match
        if (!delta) return;

        int param = m_fx_page * 4 + knob;
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, m_fx_slot) : 0;
        if (param < 0 || param >= np) return;

        // Shared accumulator keyed by (track, fx, param): a SEL knob editing the same param as a
        // dynamics knob (e.g. the surround X) reuses one running value, so the two stay in sync -
        // and press-to-reset (ResetFXParamDefault) updates that same slot.
        AccelNudge(tr, m_fx_slot, param, delta, 0.001); // finer than the surround knobs (0.01)
        LogFXParam("param", tr, m_fx_slot, param);
        if (m_window_on_knob) ShowFXWindow(); // touching a param floats the plug-in window (if enabled)
        RefreshFXDisplay();
    }

    // FX floating-window control: a param knob opens it; the PARAM button toggles it.
    // showflag: 2 = hide floating window, 3 = show floating window.
    MediaTrack *CurFXTrack()
    {
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        int n = (tr && TrackFX_GetCount) ? TrackFX_GetCount(tr) : 0;
        if (!tr || !TrackFX_Show || m_fx_slot < 0 || m_fx_slot >= n) return NULL;
        return tr;
    }
    void ShowFXWindow()   // knob-move: float the window unless it's already up
    {
        MediaTrack *tr = CurFXTrack();
        if (tr && !(TrackFX_GetFloatingWindow && TrackFX_GetFloatingWindow(tr, m_fx_slot)))
            TrackFX_Show(tr, m_fx_slot, 3);
    }
    void ToggleFXWindow() // PARAM button: show/hide the floating window
    {
        MediaTrack *tr = CurFXTrack();
        if (!tr) return;
        bool open = TrackFX_GetFloatingWindow && TrackFX_GetFloatingWindow(tr, m_fx_slot);
        TrackFX_Show(tr, m_fx_slot, open ? 2 : 3);
    }

    // Page arrows (CC 0x4C): up arrow (dir>0) = next page, down arrow (dir<0) = previous,
    // wrapping at both ends. The arrows emit the CC twice per press (~145ms apart,
    // hw-verified), so a short debounce collapses a burst into one page step.
    void OnFXPage(int dir)
    {
        if (dir == 0) return;
        DWORD now = timeGetTime();
        if (now - m_fx_page_last < 250) return;
        m_fx_page_last = now;
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        if (!tr || !TrackFX_GetCount) return;
        int n = TrackFX_GetCount(tr);
        if (m_fx_slot < 0 || m_fx_slot >= n) return;
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, m_fx_slot) : 0;
        int maxpage = np > 0 ? (np - 1) / 4 : 0;

        if (dir > 0) { if (m_fx_page < maxpage) m_fx_page++; } // up = next, stop at last page
        else         { if (m_fx_page > 0)       m_fx_page--; } // down = prev, stop at first page
        FXReport(tr);
        RefreshFXDisplay();
    }

    // Report the current FX slot / page to the console (temporary debugging aid).
    void FXReport(MediaTrack *tr)
    {
        if (!m_console_log || !ShowConsoleMsg) return;
        if (!tr) { ShowConsoleMsg("[DM2000 fx] no selected track\n"); return; }
        if (!TrackFX_GetCount) return;
        int n = TrackFX_GetCount(tr);
        if (n <= 0) { ShowConsoleMsg("[DM2000 fx] selected track has no FX\n"); return; }
        if (m_fx_slot >= n) m_fx_slot = n - 1;
        char fxname[128] = "";
        if (TrackFX_GetFXName) TrackFX_GetFXName(tr, m_fx_slot, fxname, sizeof(fxname));
        int np = TrackFX_GetNumParams ? TrackFX_GetNumParams(tr, m_fx_slot) : -1;
        char msg[360];
        sprintf_s(msg, sizeof(msg), "[DM2000 fx] slot %d/%d '%s', %d params; page %d (params %d-%d)\n",
                  m_fx_slot, n, fxname, np, m_fx_page, m_fx_page * 4, m_fx_page * 4 + 3);
        ShowConsoleMsg(msg);
    }

    // EFFECTS/PLUG-INS buttons (zone 0x1C) - the F1-F4 buttons below the display.
    // The param knobs + page arrows are always live, so these just steer which FX:
    //   F4 (sw0) = jump to FX slot 0 / page 0 and report   F2 (sw7) = next FX slot
    //   F3 (sw6) = bypass current slot                      F1 (sw1) = previous FX slot
    //   F1/F2 read left-to-right as prev/next FX (like < / > arrows).
    void OnFXSwitch(int sw)
    {
        MediaTrack *tr = GetSelectedTrack ? GetSelectedTrack(NULL, 0) : NULL;
        switch (sw)
        {
            case 0: // PARAM (F4): toggle the current FX slot's floating window on/off
                ToggleFXWindow();
                break;
            case 1: // F1 (ASSIGN): cycle to PREVIOUS FX slot (left arrow)
                if (tr && TrackFX_GetCount)
                {
                    int n = TrackFX_GetCount(tr);
                    if (n > 0) m_fx_slot = (m_fx_slot + n - 1) % n;
                    m_fx_page = 0;
                    FXReport(tr);
                }
                break;
            case 7: // F2 (COMPARE): cycle to NEXT FX slot (right arrow)
                if (tr && TrackFX_GetCount)
                {
                    int n = TrackFX_GetCount(tr);
                    if (n > 0) m_fx_slot = (m_fx_slot + 1) % n;
                    m_fx_page = 0;
                    FXReport(tr);
                }
                break;
            case 6: // F3 (BYPASS): toggle enable on the current slot
                if (tr && TrackFX_GetCount && TrackFX_GetEnabled && TrackFX_SetEnabled)
                {
                    int n = TrackFX_GetCount(tr);
                    if (m_fx_slot >= 0 && m_fx_slot < n)
                        TrackFX_SetEnabled(tr, m_fx_slot, !TrackFX_GetEnabled(tr, m_fx_slot));
                }
                break;
            case 2: case 3: case 4: case 5: // knob 1-4 press: reset that param to 0 (minimum)
                if (tr && TrackFX_GetCount)
                {
                    int n = TrackFX_GetCount(tr);
                    int param = m_fx_page * 4 + (sw - 2);
                    int np = (m_fx_slot >= 0 && m_fx_slot < n && TrackFX_GetNumParams)
                             ? TrackFX_GetNumParams(tr, m_fx_slot) : 0;
                    if (param >= 0 && param < np)
                    {
                        ResetFXParamDefault(tr, m_fx_slot, param);
                        LogFXParam("reset", tr, m_fx_slot, param);
                    }
                }
                break;
        }
        RefreshFXDisplay();
    }
};

static void GetIniPath(char *buf, int bufsz)
{
    buf[0] = '\0';
#ifdef _WIN32
    // prefer REAPER's own GetResourcePath (not in the csurf SDK headers, so look it up at runtime)
    typedef const char *(*GRP_t)();
    HMODULE hReaper = GetModuleHandleA("reaper.exe");
    GRP_t fn = hReaper ? (GRP_t)GetProcAddress(hReaper, "GetResourcePath") : NULL;
    if (fn)
        sprintf_s(buf, bufsz, "%s\\dm2000_keys.ini", fn());
    else
    {
        char appdata[MAX_PATH] = "";
        SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata);
        sprintf_s(buf, bufsz, "%s\\REAPER\\dm2000_keys.ini", appdata);
    }
#else
    // macOS: GetResourcePath (resolved via rec->GetFunc in csurf_main) returns
    // ~/Library/Application Support/REAPER; the keys file lives alongside reaper.ini.
    // No GetModuleHandle on SWELL, so this is the only portable resolver.
    if (GetResourcePath)
        sprintf_s(buf, bufsz, "%s/dm2000_keys.ini", GetResourcePath());
#endif
}

// parms[0..7] = 4 HUI in/out device pairs; parms[8]/parms[9] = GENERAL in/out
// (-1 = none). The GENERAL pair is appended for scene recall; older 8-int config
// strings parse fine and leave it at -1 (disabled).
static void parseParms(const char *str, int parms[10], char *iniPath, int iniPathSz)
{
    for (int i = 0; i < 10; ++i) parms[i] = -1;
    if (iniPath) iniPath[0] = '\0';

    const char *p = str;
    if (p)
    {
        int x = 0;
        while (x < 10 && *p)
        {
            while (*p == ' ') p++;
            if (*p == '|' || ((*p < '0' || *p > '9') && *p != '-')) break;
            parms[x++] = atoi(p);
            while (*p && *p != ' ' && *p != '|') p++;
        }
        if (*p == '|' && iniPath)
            sprintf_s(iniPath, iniPathSz, "%s", p + 1);
    }
}

static IReaperControlSurface *createFunc(const char *type_string, const char *configString, int *errStats)
{
    int parms[10];
    char iniPath[MAX_PATH] = "";
    parseParms(configString, parms, iniPath, sizeof(iniPath));

    return new CSurf_DM2000(parms[0], parms[1], parms[2], parms[3], parms[4], parms[5], parms[6], parms[7], parms[8], parms[9], iniPath, errStats);
}

static WDL_DLGRET dlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            int parms[10];
            char iniPath[MAX_PATH] = "";
            parseParms((const char *)lParam, parms, iniPath, sizeof(iniPath));
            if (!iniPath[0]) GetIniPath(iniPath, sizeof(iniPath));

            // port group combo: consecutive 4-port groups from MIDI inputs
            int n = GetNumMIDIInputs();
            int x = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_ADDSTRING, 0, (LPARAM)"None");
            SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETITEMDATA, x, -1);
            SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETCURSEL, x, 0);
            for (int j = 0; j + 3 < n; ++j)
            {
                char first[256], last[256];
                if (GetMIDIInputName(j, first, sizeof(first)) && GetMIDIInputName(j + 3, last, sizeof(last)))
                {
                    char buf[600];
                    sprintf(buf, "%s ... %s", first, last);
                    int a = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_ADDSTRING, 0, (LPARAM)buf);
                    SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETITEMDATA, a, j);
                    if (j == parms[0]) SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETCURSEL, a, 0);
                }
            }

            // GENERAL port combo: a single arbitrary MIDI input (the console's GENERAL
            // Rx/Tx port) for optional scene recall. "None" leaves it disabled.
            int gx = SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_ADDSTRING, 0, (LPARAM)"None");
            SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_SETITEMDATA, gx, -1);
            SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_SETCURSEL, gx, 0);
            for (int j = 0; j < n; ++j)
            {
                char nm[256];
                if (GetMIDIInputName(j, nm, sizeof(nm)))
                {
                    int a = SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_ADDSTRING, 0, (LPARAM)nm);
                    SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_SETITEMDATA, a, j);
                    if (j == parms[8]) SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_SETCURSEL, a, 0);
                }
            }

            SetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath);
#ifndef _WIN32
            {
                // Set link text color to NSColor.linkColor (system blue, dark-mode aware).
                // WM_CTLCOLORSTATIC in SWELL applies one color to all statics, so we use
                // objc_msgSend directly to target only the two link controls.
                typedef id   (*MSId)(id, SEL);
                typedef void (*MSVoidId)(id, SEL, id);
                id cls = (id)objc_getClass("NSColor");
                id col = ((MSId)objc_msgSend)(cls, sel_registerName("linkColor"));
                SEL setCol = sel_registerName("setTextColor:");
                id c1 = (id)GetDlgItem(hwndDlg, IDC_DM2000_LINK);
                id c2 = (id)GetDlgItem(hwndDlg, IDC_DM2000_INIEXAMPLE);
                if (c1) ((MSVoidId)objc_msgSend)(c1, setCol, col);
                if (c2) ((MSVoidId)objc_msgSend)(c2, setCol, col);
            }
#endif
        }
        break;
        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_OPENFOLDER)
            {
                char iniPath[MAX_PATH];
                GetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath, sizeof(iniPath));
                char folder[MAX_PATH];
                strcpy_s(folder, sizeof(folder), iniPath);
                char *sep = strrchr(folder, DM2000_PATHSEP);
                if (sep) *sep = '\0';
#ifdef _WIN32
                ShellExecuteA(hwndDlg, "explore", folder, NULL, NULL, SW_SHOWNORMAL);
#else
                // SWELL routes a bare directory path to NSWorkspace openFile (opens in Finder).
                ShellExecute(hwndDlg, "open", folder, NULL, NULL, 0);
#endif
            }
        }
        break;
#ifndef _WIN32
        case WM_MOUSEMOVE:
        {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            auto overLink = [&](int ctrlId) -> bool {
                HWND h = GetDlgItem(hwndDlg, ctrlId);
                RECT rc; GetWindowRect(h, &rc);
                ScreenToClient(hwndDlg, (LPPOINT)&rc);
                ScreenToClient(hwndDlg, (LPPOINT)&rc + 1);
                return !!PtInRect(&rc, pt);
            };
            SetCursor(LoadCursor(NULL,
                (overLink(IDC_DM2000_LINK) || overLink(IDC_DM2000_INIEXAMPLE))
                ? IDC_HAND : IDC_ARROW));
        }
        break;
        case WM_LBUTTONDOWN:
        {
            // SWELL does not fire WM_COMMAND for SS_NOTIFY statics, so hit-test manually.
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            HWND hwndLink = GetDlgItem(hwndDlg, IDC_DM2000_LINK);
            RECT rcLink;
            GetWindowRect(hwndLink, &rcLink);
            ScreenToClient(hwndDlg, (LPPOINT)&rcLink);
            ScreenToClient(hwndDlg, (LPPOINT)&rcLink + 1);
            if (PtInRect(&rcLink, pt))
            {
                ShellExecute(hwndDlg, "open", "https://bmroz.eu/projects/dm2000-csurf", NULL, NULL, 0);
                return 1;
            }
            HWND hwndIni = GetDlgItem(hwndDlg, IDC_DM2000_INIEXAMPLE);
            RECT rcIni;
            GetWindowRect(hwndIni, &rcIni);
            ScreenToClient(hwndDlg, (LPPOINT)&rcIni);
            ScreenToClient(hwndDlg, (LPPOINT)&rcIni + 1);
            if (PtInRect(&rcIni, pt))
            {
                ShellExecute(hwndDlg, "open", "https://github.com/mormegil6/reaper-dm2000-csurf/blob/main/doc/dm2000_keys.ini.example", NULL, NULL, 0);
                return 1;
            }
        }
        break;
#endif
        case WM_USER + 1024:
            if (wParam > 1 && lParam)
            {
                char tmp[MAX_PATH + 128];

                int indevs[4] = { -1, -1, -1, -1 }, outdevs[4] = { -1, -1, -1, -1 };
                int start = -1;
                int r = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_GETCURSEL, 0, 0);
                if (r != CB_ERR) start = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_GETITEMDATA, r, 0);

                if (start >= 0)
                {
                    // output devices are enumerated separately from inputs, so the
                    // same physical port can have a different index in each list --
                    // locate the output port by name instead of reusing the input index
                    int outstart = start;
                    char inname[256];
                    if (GetMIDIInputName(start, inname, sizeof(inname)))
                    {
                        int nout = GetNumMIDIOutputs();
                        for (int j = 0; j < nout; ++j)
                        {
                            char outname[256];
                            if (GetMIDIOutputName(j, outname, sizeof(outname)) && !strcmp(outname, inname))
                            {
                                outstart = j;
                                break;
                            }
                        }
                    }

                    for (int i = 0; i < 4; ++i)
                    {
                        indevs[i] = start + i;
                        outdevs[i] = outstart + i;
                    }
                }

                // GENERAL port: the selected MIDI input index; its output is located
                // by name (output list is enumerated separately from inputs).
                int genin = -1, genout = -1;
                int gr = SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_GETCURSEL, 0, 0);
                if (gr != CB_ERR) genin = SendDlgItemMessage(hwndDlg, IDC_COMBO_GENERAL, CB_GETITEMDATA, gr, 0);
                if (genin >= 0)
                {
                    char gname[256];
                    if (GetMIDIInputName(genin, gname, sizeof(gname)))
                    {
                        int nout = GetNumMIDIOutputs();
                        for (int j = 0; j < nout; ++j)
                        {
                            char outname[256];
                            if (GetMIDIOutputName(j, outname, sizeof(outname)) && !strcmp(outname, gname))
                            {
                                genout = j;
                                break;
                            }
                        }
                    }
                }

                char iniPath[MAX_PATH] = "";
                GetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath, sizeof(iniPath));
                if (iniPath[0])
                    sprintf(tmp, "%d %d %d %d %d %d %d %d %d %d|%s", indevs[0], outdevs[0], indevs[1], outdevs[1], indevs[2], outdevs[2], indevs[3], outdevs[3], genin, genout, iniPath);
                else
                    sprintf(tmp, "%d %d %d %d %d %d %d %d %d %d", indevs[0], outdevs[0], indevs[1], outdevs[1], indevs[2], outdevs[2], indevs[3], outdevs[3], genin, genout);
                lstrcpyn((char *)lParam, tmp, wParam);
            }
        break;
#ifdef _WIN32
        // SWELL has no WHITE_BRUSH and its GetStockObject only knows NULL_BRUSH/NULL_PEN,
        // so this would not compile on macOS; SWELL edit fields render white by default.
        case WM_CTLCOLOREDIT:
            if (GetDlgCtrlID((HWND)lParam) == IDC_EDIT_INIPATH)
            {
                SetBkColor((HDC)wParam, RGB(255, 255, 255));
                return (INT_PTR)GetStockObject(WHITE_BRUSH);
            }
        break;
        // SysLink + NMLINK exist only on Windows; on macOS the footer is an SS_NOTIFY
        // static handled via WM_COMMAND/IDC_DM2000_LINK above (NMLINK is undefined in SWELL).
        case WM_NOTIFY:
        {
            NMHDR *hdr = (NMHDR *)lParam;
            if (hdr->code == NM_CLICK || hdr->code == NM_RETURN)
            {
                NMLINK *link = (NMLINK *)lParam;
                ShellExecuteW(hwndDlg, L"open", link->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
            }
        }
        break;
#endif
    }
    return 0;
}

static HWND configFunc(const char *type_string, HWND parent, const char *initConfigString)
{
    return CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_SURFACEEDIT_DM2000), parent, dlgProc, (LPARAM)initConfigString);
}

reaper_csurf_reg_t csurf_dm2000_reg =
{
  "DM2000",
  "Yamaha DM2000",
  createFunc,
  configFunc,
};
