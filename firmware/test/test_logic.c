/* Host test for the simple 6-byte protocol. */
#include <stdio.h>
#include "../common/proto.h"
int main(void)
{
    int fails = 0;
    pkt_t p = { PKT_BUTTONS, 0xfe98be33u, 2 }, q;
    uint8_t b[PKT_LEN];
    pkt_build(&p, b);
    if (!pkt_parse(b, PKT_LEN, &q) || q.id != p.id || q.buttons != 2 || q.type != PKT_BUTTONS) { puts("FAIL roundtrip"); fails++; }
    if (pkt_parse(b, PKT_LEN - 1, &q)) { puts("FAIL short len accepted"); fails++; }
    b[5] = 0x80; if (pkt_parse(b, PKT_LEN, &q)) { puts("FAIL bad buttons accepted"); fails++; }
    b[5] = 1; b[0] = 7; if (pkt_parse(b, PKT_LEN, &q)) { puts("FAIL bad type accepted"); fails++; }
    puts(fails ? "PROTO TESTS FAILED" : "proto: all tests OK");
    return fails;
}
