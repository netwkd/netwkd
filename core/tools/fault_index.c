#include <stdio.h>

#include "../netw_faults.h"

#define NETW_FAULT_ROW(m_name, m_id, m_class, m_aud, m_subj, m_site, m_fwd) \
    printf("- :doc:`nw%04d`, %s\n", m_id, #m_class);

#define NETW_FAULT_TOC(m_name, m_id, m_class, m_aud, m_subj, m_site, m_fwd) \
    printf("   nw%04d\n", m_id);

int main(void) {
    puts("Fault index\n===========\n");
    NETW_FAULTS(NETW_FAULT_ROW)
    puts("\n.. toctree::\n   :hidden:\n");
    NETW_FAULTS(NETW_FAULT_TOC)
    return 0;
}
