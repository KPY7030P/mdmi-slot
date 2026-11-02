// Bundles the BlastEm core and the MDMI ROM into the plugin binary, so it installs as one file.
#define STR2(x) #x
#define STR(x) STR2(x)

#define EMBED(symbol, path) \
	__asm__( \
		".section .rdata,\"dr\"\n" \
		".global " #symbol "\n" \
		".global " #symbol "_end\n" \
		".balign 16\n" \
		#symbol ":\n" \
		".incbin \"" path "\"\n" \
		#symbol "_end:\n" \
		".text\n" \
	)

EMBED(mdmi_blob_engine, STR(MDMI_ENGINE_PATH));
EMBED(mdmi_blob_rom, STR(MDMI_ROM_PATH));
