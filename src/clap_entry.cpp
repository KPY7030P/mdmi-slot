// Compiled once per plugin format by clap-wrapper's make_clapfirst_plugins.
#include <clap/clap.h>

extern "C" {
bool mdmi_clap_init(const char *plugin_path);
void mdmi_clap_deinit(void);
const void *mdmi_clap_get_factory(const char *factory_id);

CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;
const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT,
	mdmi_clap_init,
	mdmi_clap_deinit,
	mdmi_clap_get_factory,
};
}
