/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DS's Wi-Fi Connection (WFC) through a replacement service: the services' DNS servers, which answer the
   service's names (nas.nintendowifi.net, conntest.nintendowifi.net, *.gs.nintendowifi.net...) with their own
   servers, and the firmware's connection settings that make a DS ask them, as a player sets them by hand on the
   console. README.md here lists each service and how its server was checked */
#ifndef SNX_ONLINE_WFC_H
#define SNX_ONLINE_WFC_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnxOnlineWfcPreset {
    const char* id;   /* the value a setting keeps: lower case letters, digits and _ */
    const char* name; /* the service's own name, for a menu */
    uint8_t dns[4];   /* its DNS server's IPv4 address */
} SnxOnlineWfcPreset;

/* the services, in the order a menu lists them */
size_t snx_online_wfc_preset_count(void);
const SnxOnlineWfcPreset* snx_online_wfc_preset_at(size_t index);     /* NULL past the end */
const SnxOnlineWfcPreset* snx_online_wfc_preset_find(const char* id); /* NULL for an id no service has */

/* The DNS server for a choice: a service's id, "custom" with custom an IPv4 address, "auto" (the service that answers
   fastest, snx_online_wfc_fastest), or "off" (the DS takes the one its network gives it). 1 with dns filled,
   SNX_ONLINE_WFC_AUTO for auto (dns left as it was: the caller measures), 0 for off, -1 with why saying the value given
   and what is taken */
#define SNX_ONLINE_WFC_AUTO 2
int snx_online_wfc_dns(const char* choice, const char* custom, uint8_t dns[4], char* why, size_t size);

/* The server that answers fastest: one query for nas.nintendowifi.net to each of count servers at once (ports NULL:
   53 for each), the answers waited for up to timeout_ms in all. ms[i] is the milliseconds server i took, -1 when it
   gave no address in time. The fastest one's index, or -1 with why when none answered */
int snx_online_wfc_fastest(const uint8_t (*servers)[4], const uint16_t* ports, size_t count, int timeout_ms, int* ms,
                           char* why, size_t size);

/* a dotted quad of four decimal numbers 0 to 255, nothing around it: 1 with out filled, else 0 */
int snx_online_ipv4_parse(const char* text, uint8_t out[4]);

/* The DS firmware's Wi-Fi connection settings: three slots of 256 bytes, the first at the user settings' offset less
   0x400 (0x3FA00 in a 256 KiB flash), as GBATEK's "DS Firmware Wifi Internet Access Points" lays them out */
#define SNX_ONLINE_NDS_WFC_SLOT_SIZE   256u
#define SNX_ONLINE_NDS_WFC_SLOTS       3u
#define SNX_ONLINE_NDS_WFC_BEFORE_USER 0x400u

/* the firmware's CRC-16 (GBATEK's, the reflected 0xA001 polynomial), from crc over length bytes */
uint16_t snx_online_nds_crc16(uint16_t crc, const uint8_t* bytes, size_t length);

/* A slot configured for an open network named ssid (1 to 32 bytes) whose addresses come from its DHCP, with the DNS
   servers dns1 and dns2 (0.0.0.0 for the network's own), status normal, its CRC set. 0, or -1 with why */
int snx_online_nds_wfc_slot(uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE], const char* ssid, const uint8_t dns1[4],
                            const uint8_t dns2[4], char* why, size_t size);
/* a slot no connection is set in: zeros, status 0xFF, its CRC set */
void snx_online_nds_wfc_slot_empty(uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE]);

typedef struct SnxOnlineNdsWfcSlot {
    char ssid[33];
    uint8_t dns1[4], dns2[4];
} SnxOnlineNdsWfcSlot;
/* A slot read back: 1 with out filled for a configured one, 0 for one with no connection set, -1 with why when its
   CRC does not match its bytes */
int snx_online_nds_wfc_slot_read(const uint8_t slot[SNX_ONLINE_NDS_WFC_SLOT_SIZE], SnxOnlineNdsWfcSlot* out, char* why,
                                 size_t size);

#ifdef __cplusplus
}
#endif
#endif
