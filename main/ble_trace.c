// Diagnostic build only. Inspect packet metadata, then forward unchanged exactly once.
#include "v3.h"
#include "os/os_mbuf.h"
#include <stdatomic.h>
struct ble_hs_conn;
int __real_ble_hs_hci_evt_acl_process(struct os_mbuf *);
int __real_ble_hs_hci_acl_tx(struct ble_hs_conn *, struct os_mbuf **);
static atomic_uint rx_acl, tx_acl, rx_att, tx_att, rx_mtu, tx_mtu;
static atomic_uint rx_group, tx_group, rx_type, tx_type, rx_smp, tx_smp;
static atomic_uint rx_errors, tx_errors, rx_last, tx_last;
static void inspect(struct os_mbuf *om, bool rx) {
    uint8_t p[9];
    size_t n = rx ? 9 : 5;
    if (!om || os_mbuf_copydata(om, 0, n, p)) return;
    unsigned offset = rx ? 4 : 0;
    if (rx && ((p[1] >> 4) & 3) == 1) return; // HCI continuation, not a new L2CAP header.
    unsigned cid = p[offset + 2] | ((unsigned)p[offset + 3] << 8);
    if (cid == 6) atomic_fetch_add(rx ? &rx_smp : &tx_smp, 1);
    if (cid != 4) return;
    unsigned opcode = p[offset + 4];
    atomic_store(rx ? &rx_last : &tx_last, opcode);
    atomic_fetch_add(rx ? &rx_att : &tx_att, 1);
    if (opcode == (rx ? 2 : 3)) atomic_fetch_add(rx ? &rx_mtu : &tx_mtu, 1);
    if (opcode == (rx ? 16 : 17)) atomic_fetch_add(rx ? &rx_group : &tx_group, 1);
    if (opcode == (rx ? 8 : 9)) atomic_fetch_add(rx ? &rx_type : &tx_type, 1);
}
int __wrap_ble_hs_hci_evt_acl_process(struct os_mbuf *om) {
    atomic_fetch_add(&rx_acl, 1);
    inspect(om, true);
    int rc = __real_ble_hs_hci_evt_acl_process(om);
    if (rc) atomic_fetch_add(&rx_errors, 1);
    return rc;
}
int __wrap_ble_hs_hci_acl_tx(struct ble_hs_conn *conn, struct os_mbuf **om) {
    atomic_fetch_add(&tx_acl, 1);
    inspect(om ? *om : NULL, false);
    int rc = __real_ble_hs_hci_acl_tx(conn, om);
    if (rc) atomic_fetch_add(&tx_errors, 1);
    return rc;
}
void transport_ble_trace(cJSON *parent) {
    cJSON *p = cJSON_AddObjectToObject(parent, "ble_trace");
#define ITEM(name) cJSON_AddNumberToObject(p, #name, atomic_load(&name))
    ITEM(rx_acl); ITEM(tx_acl); ITEM(rx_att); ITEM(tx_att); ITEM(rx_mtu); ITEM(tx_mtu);
    ITEM(rx_group); ITEM(tx_group); ITEM(rx_type); ITEM(tx_type); ITEM(rx_smp); ITEM(tx_smp);
    ITEM(rx_errors); ITEM(tx_errors); ITEM(rx_last); ITEM(tx_last);
#undef ITEM
}
