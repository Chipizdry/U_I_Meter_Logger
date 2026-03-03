


#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/dns.h"
#include "esp_log.h"

static struct udp_pcb *dns_pcb;
static ip_addr_t ap_ip;

static void dns_recv_cb(
    void *arg,
    struct udp_pcb *pcb,
    struct pbuf *p,
    const ip_addr_t *addr,
    u16_t port)
{
    if (!p) return;

    uint16_t req_len = p->tot_len;
    if (req_len < 12) {
        pbuf_free(p);
        return;
    }

    struct pbuf *response = pbuf_alloc(PBUF_TRANSPORT, 512, PBUF_RAM);
    if (!response) {
        pbuf_free(p);
        return;
    }

    uint8_t *dns = (uint8_t *)response->payload;
    memset(dns, 0, 512);

    // ===== HEADER =====
    pbuf_copy_partial(p, dns, req_len, 0);

    dns[2] = 0x81; // QR=1, Opcode=0
    dns[3] = 0x80; // RA
    dns[4] = 0x00; dns[5] = 0x01; // QDCOUNT
    dns[6] = 0x00; dns[7] = 0x01; // ANCOUNT

    int pos = req_len;

    // ===== ANSWER =====
    dns[pos++] = 0xC0; dns[pos++] = 0x0C;
    dns[pos++] = 0x00; dns[pos++] = 0x01;
    dns[pos++] = 0x00; dns[pos++] = 0x01;
    dns[pos++] = 0x00; dns[pos++] = 0x00;
    dns[pos++] = 0x00; dns[pos++] = 0x3C;
    dns[pos++] = 0x00; dns[pos++] = 0x04;

    memcpy(&dns[pos], &ap_ip.u_addr.ip4.addr, 4);
    pos += 4;

    response->len = response->tot_len = pos;

    udp_sendto(pcb, response, addr, port);

    pbuf_free(response);
    pbuf_free(p);
}

void dns_start(void)
{
    dns_pcb = udp_new();

    // IP ТОЛЬКО AP
    IP4_ADDR(&ap_ip.u_addr.ip4, 192, 168, 4, 1);
    ap_ip.type = IPADDR_TYPE_V4;

    // 🔒 биндимся ТОЛЬКО к AP IP
   // udp_bind(dns_pcb, &ap_ip, 53);
    udp_bind(dns_pcb, IP_ADDR_ANY, 53);

    udp_recv(dns_pcb, dns_recv_cb, NULL);
    ESP_LOGI("DNS", "Captive DNS started on AP only");
}
void dns_stop(void)
{
    if (dns_pcb) {
        udp_remove(dns_pcb);
        dns_pcb = NULL;
        ESP_LOGI("DNS", "Captive DNS stopped");
    }
}


