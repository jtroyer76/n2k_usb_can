// While the setup hotspot is on, answers every DNS lookup with the gateway's
// address (SETUP_IP), so a phone's "is there internet?" check lands on the setup page and the
// phone opens it by itself (a captive portal).
#pragma once

void captive_dns_start(void);
void captive_dns_stop(void);
