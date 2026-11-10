#include <Airport/experimental/IO80211VirtualInterfaceABI_25G332.h>

class ProbeVIF : public IO80211VirtualInterface {
public:
    OSDeclareDefaultStructors(ProbeVIF)
    uintptr_t disable(unsigned int) override { return 0; }
    uintptr_t initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**) override { return 0; }
    uintptr_t setMacAddress(ether_addr&) override { return 0; }
};
OSDefineMetaClassAndStructors(ProbeVIF, IO80211VirtualInterface)
ProbeVIF *g_probe_vif;
