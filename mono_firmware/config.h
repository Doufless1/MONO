/* ============================================================================
   config.h  -  everything you might want to change, in one place
   ----------------------------------------------------------------------------
   The sketch, the host test harness, the terminal simulator and the WebAssembly
   build all include this file, so a value changed here changes everywhere.
   ========================================================================= */

#ifndef MONO_CONFIG_H
#define MONO_CONFIG_H

#define MONO_FW_VERSION "3.0.0"
#define MONO_PROTOCOL   3

/* ------------------------------------------------------------ build flags --
   MONO_BENCH
     1 = the USB console may run every command at any time. Convenient at the
         bench, and a hole in a pocket: the USB-C port is on the outside of the
         wallet, so a thief with a cable could type UNLOCK. Never carry a
         bench build. 0 (default) = the console is trusted only while the
         wallet is factory-fresh, or for a minute after the owner's finger
         opened it.

   MONO_LINK_ENCRYPTION
     1 = the UART service refuses unencrypted links, so the phone's OS pairs
         (LE Secure Connections, Just Works) before the app can say anything.
         That stops a passive sniffer from reading the one-time key in PAIR.
         Off by default only because the host harness cannot exercise it:
         turn it on, then check your phone shows the pairing dialog once.  */
#ifndef MONO_BENCH
#define MONO_BENCH 0
#endif
#ifndef MONO_LINK_ENCRYPTION
#define MONO_LINK_ENCRYPTION 0
#endif

/* ------------------------------------------------------------------- pins --
   R502 red    3V3  -> 3V3          R502 yellow TX -> D7 (XIAO RX)
   R502 black  GND  -> GND          R502 green  RX -> D6 (XIAO TX)
   R502 blue   WAKE -> D2           R502 white  Vt -> 3V3  (touch power)
   servo signal     -> D3           buzzer +       -> D4
   status LED +     -> D5 via 330R  LiPo           -> BAT pads (check polarity!) */
#define PIN_FP_WAKE     2
#define PIN_SERVO       3
#define PIN_BUZZER      4
#define PIN_STATUS_LED  5   /* not PIN_LED: the variant defines that (the onboard
                               RGB's red), and LED_BUILTIN through it            */
#ifndef VBAT_ENABLE
#define VBAT_ENABLE 14   /* P0.14: LOW connects the battery divider to P0.31  */
#endif
#ifndef PIN_VBAT
#define PIN_VBAT    32   /* P0.31 / AIN7 in the Seeed variant's numbering     */
#endif

/* ----------------------------------------------------------------- timing -- */
#define SCAN_WINDOW_MS       8000UL   /* give up on a touch that never images  */
#define REJECT_HOLD_MS       4000UL   /* wait this long for a wrong finger to lift */
#define LOCKOUT_MS          30000UL   /* sensor ignored after maxFails wrong fingers */
#define ENROLL_STEP_MS      20000UL   /* each enrolment step times out after this */
#define SERVO_SETTLE_MS       350UL   /* servo powered this long per move, then detached */
#define TAMPER_SAMPLE_MS       20UL   /* IMU sampled at a fixed 50 Hz, whatever the loop does */
#define TAMPER_KNOCKS           3     /* this many separate knocks ...          */
#define TAMPER_WINDOW_MS     1500UL   /* ... inside this window = someone has it */
#define ALARM_BURST_EVERY_MS 2500UL   /* lockdown beeps a burst this often ...  */
#define ALARM_SOUND_MS      60000UL   /* ... until a minute after the last shake */
#define PAIR_CONFIRM_MS     20000UL   /* the owner has this long to touch and confirm a new phone */
#define OWNER_WINDOW_MS     60000UL   /* USB console trusted this long after a match */
#define BATT_PERIOD_MS      60000UL   /* battery re-read this often             */
#define IDLE_MAX_MS            20UL   /* longest the loop ever sleeps           */

/* ---------------------------------------------------------------- radio -- */
#define ADV_NORMAL        244   /* x0.625 ms = ~152 ms                         */
#define ADV_FAST           32   /* x0.625 ms =   20 ms, alarm and separation   */
#define FINDMY_SLICE_MS  8000UL /* beacon slice when time-slicing with the app */
#define APPLINK_SLICE_MS 8000UL /* connectable slice                           */

/* ----------------------------------------------------------- capacities -- */
#define MAX_FINGERS     10      /* MONO manages template ids 1..MAX_FINGERS    */
#define MAX_PHONES       4      /* paired phone keys kept on the wallet        */
#define PHONE_KEY_LEN   16      /* bytes; HMAC-SHA256 key per phone            */
#define NONCE_LEN       16      /* bytes; one fresh challenge per HELLO        */
#define CMD_LINE_MAX    96      /* longest command line, terminator included (not
                                   LINE_MAX: glibc's <limits.h> owns that name)  */
#define OUT_MAX        168      /* longest line the wallet ever sends          */

#endif
