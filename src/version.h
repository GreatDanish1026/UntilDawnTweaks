// Single source of the product name and version number (add-on, version resource).
#pragma once

#define UDRT_PRODUCT "UntilDawnTweaks"  // add-on name, log file prefix, ReShade.ini section
#define UDRT_LEGACY_PRODUCT "UntilDawnRT"  // the name before 0.0.7: its add-on file and ini section
#define UDRT_GAME "Until Dawn"
#define UDRT_VERSION_MAJOR 1
#define UDRT_VERSION_MINOR 0
#define UDRT_VERSION_PATCH 0
#define UDRT_VERSION "1.0.0"
#define UDRT_RC_FILENAME "UntilDawnTweaks.addon64"
#define UDRT_RC_PRODUCT "UntilDawnTweaks for Until Dawn"
#define UDRT_RC_DESCRIPTION "UntilDawnTweaks ReShade add-on"

// PE timestamp of the Bates-Win64-Shipping.exe whose PDB the layouts were read from.
#define UDRT_KNOWN_PE_TIMESTAMP 0xcc7d6d54u
