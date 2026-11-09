/* SPDX-License-Identifier: GPL-2.0-only */
/* Spring update rules and payload digests from the verified reference. */
#define HG_128G_156_SHA256 { \
	0xc6, 0xd7, 0x04, 0xc6, 0x0f, 0x49, 0x0f, 0x94, \
	0xc2, 0x70, 0xaf, 0xe9, 0xbd, 0xc4, 0x30, 0xaa, \
	0xf8, 0x69, 0x6f, 0x3b, 0x3c, 0x77, 0x61, 0xb0, \
	0x94, 0x71, 0x07, 0x2a, 0x4d, 0xab, 0x56, 0xf2, \
}

#define HG_128G_50B_SHA256 { \
	0xf6, 0xed, 0x66, 0xae, 0x50, 0x79, 0x6e, 0x0e, \
	0xa4, 0x35, 0x46, 0xfb, 0x11, 0x78, 0xb6, 0x4b, \
	0x72, 0x06, 0x10, 0xbf, 0xd2, 0xe9, 0xa8, 0x57, \
	0x0e, 0x56, 0x2c, 0xc1, 0x11, 0xe9, 0xe0, 0x18, \
}

#define HG_256G_156_SHA256 { \
	0x23, 0x0f, 0xf2, 0x25, 0x90, 0xfe, 0x33, 0x00, \
	0xbc, 0x0b, 0x8c, 0x96, 0x78, 0xcf, 0x7d, 0x47, \
	0x28, 0xc5, 0xbc, 0xa9, 0xea, 0x6f, 0x27, 0x2a, \
	0x77, 0x7a, 0x17, 0x57, 0xb3, 0x56, 0x82, 0x9b, \
}

#define XBSTOR_0315_SHA256 { \
	0x2e, 0x06, 0xa8, 0x81, 0x52, 0x1f, 0x0c, 0x53, \
	0x1c, 0x13, 0x4a, 0xe5, 0x6a, 0xb2, 0x80, 0x09, \
	0xfc, 0x99, 0x58, 0xe3, 0x85, 0x82, 0x71, 0x4b, \
	0x8d, 0xdd, 0xb1, 0x7d, 0x6c, 0x05, 0x63, 0xae, \
}

static const struct spring_ffu_update spring_ffu_updates[] = {
	{ "HG", "CBE06-1280CHAC", "00.0", "50.0", "HG_128G_50B.bin", 696320,
	  HG_128G_50B_SHA256 },
	{ "HG", "CBE06-1280CHAC", "50.0", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-1280CHAC", "13.0", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-1280CHAC", "14.2", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-1280CHAC", "15.0", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-1280CHAC", "15.2", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-1280CHAC", "15.4", "15.6", "HG_128G_156.bin", 696320,
	  HG_128G_156_SHA256 },
	{ "HG", "CBE06-2560CHAC", "13.0", "15.6", "HG_256G_156.bin", 696320,
	  HG_256G_156_SHA256 },
	{ "HG", "CBE06-2560CHAC", "14.2", "15.6", "HG_256G_156.bin", 696320,
	  HG_256G_156_SHA256 },
	{ "HG", "CBE06-2560CHAC", "15.0", "15.6", "HG_256G_156.bin", 696320,
	  HG_256G_156_SHA256 },
	{ "HG", "CBE06-2560CHAC", "15.2", "15.6", "HG_256G_156.bin", 696320,
	  HG_256G_156_SHA256 },
	{ "HG", "CBE06-2560CHAC", "15.4", "15.6", "HG_256G_156.bin", 696320,
	  HG_256G_156_SHA256 },
	{ "XBSTOR", "XBUSC1A18A8TG2", "0301", "0315", "XBSTOR_0315.bin", 692224,
	  XBSTOR_0315_SHA256 },
	{ "XBSTOR", "XBUSC1A18A8TG2", "0306", "0315", "XBSTOR_0315.bin", 692224,
	  XBSTOR_0315_SHA256 },
	{ "XBSTOR", "XBUSC1A18A8TG2", "0309", "0315", "XBSTOR_0315.bin", 692224,
	  XBSTOR_0315_SHA256 },
	{ "XBSTOR", "XBUSC1A19A8TG3", "0306", "0315", "XBSTOR_0315.bin", 692224,
	  XBSTOR_0315_SHA256 },
	{ "XBSTOR", "XBUSC1A19A8TG3", "0309", "0315", "XBSTOR_0315.bin", 692224,
	  XBSTOR_0315_SHA256 },
};
