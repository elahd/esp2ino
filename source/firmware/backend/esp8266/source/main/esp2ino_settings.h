/**
 * Flags & Options
 * */

/**
 * TO-DO: Trigger CMAKE warning when dev flags are enabled.
 */

/** DEVELOPMENT FLAGS **/
/** These must all be false before publishing release! **/

#define DEV_FAKE_WRITE false       // Skips actual OTA task. Defaults to success.
#define DEV_FORCE_FAIL false       // Forces a failure on bootloader erase step.
#define DEV_FORCE_SAFEMODE false   // Force booting into safe mode.
#define DEV_WIFI_AUTOCONNECT false // Automatically connect to Wi-Fi using credentials stored in NVS.

/** FEATURE TOGGLES **/
#define FEAT_DEBUG_LOG_TO_WEBUI false   // See note before webui_debugPub definition, below.
#define FEAT_MODERN_WIFI_ONLY false     // Only connect to WPA2 networks.
#define FEAT_MAC_IN_MDNS_HOSTNAME false // Include MAC address in mDNS hostname (esp2ino-fa348d vs esp2ino)