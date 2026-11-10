#include <Airport/experimental/IO80211SkywalkABI_25G332.h>

class ProbeSky : public IO80211SkywalkInterface {
public:
    OSDeclareDefaultStructors(ProbeSky)
    uintptr_t enable(unsigned int) override { return 0; }
    uintptr_t disable(unsigned int) override { return 0; }
    uintptr_t initBSDInterfaceParameters(ifnet_init_eparams*, sockaddr_dl**) override { return 0; }
};
OSDefineMetaClassAndStructors(ProbeSky, IO80211SkywalkInterface)
ProbeSky *g_probe_sky;
