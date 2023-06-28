#include <stdarg.h>
#define FMT_HEADER_ONLY
#include "settings.h"

char *commandLineOptions() {
    return "hactool (c) SciresM.\n"
        "Built: %s %s\n"
        "\n"
        "Usage: %s [options...] <file>\n"
        "Options:\n"
        "  -i, --info         Show file info.\n"
        "                         This is the default action.\n"
        "  -x, --extract      Extract data from file.\n"
        "                         This is also the default action.\n"
        "  -f, --file=name    Extract a single file.\n"
        "  -r, --raw          Keep raw data, don't unpack.\n"
        "  -y, --verify       Verify hashes and signatures.\n"
        "  -d, --dev          Decrypt with development keys instead of retail.\n"
        "  -k, --keyset       Load keys from an external file.\n"
        "  -t, --intype=type  Specify input file type [nca, xci, pfs0, romfs, hfs0, npdm, pk11, pk21, ini1, kip1, nax0, save, keygen]\n"
        "  -o, --outtype=type Specify the output type of the text printed to your console. [default, silent, json]\n"
        "  --titlekey=key     Set title key for Rights ID crypto titles.\n"
        "  --contentkey=key   Set raw key for NCA body decryption.\n"
        "  --disablekeywarns  Disables warning output when loading external keys.\n"
        "NCA options:\n"
        "  --plaintext=file   Specify file path for saving a decrypted copy of the NCA.\n"
        "  --header=file      Specify Header file path.\n"
        "  --section0=file    Specify Section 0 file path.\n"
        "  --section1=file    Specify Section 1 file path.\n"
        "  --section2=file    Specify Section 2 file path.\n"
        "  --section3=file    Specify Section 3 file path.\n"
        "  --section0dir=dir  Specify Section 0 directory path.\n"
        "  --section1dir=dir  Specify Section 1 directory path.\n"
        "  --section2dir=dir  Specify Section 2 directory path.\n"
        "  --section3dir=dir  Specify Section 3 directory path.\n"
        "  --exefs=file       Specify ExeFS file path. Overrides appropriate section file path.\n"
        "  --exefsdir=dir     Specify ExeFS directory path. Overrides appropriate section directory path.\n"
        "  --romfs=file       Specify RomFS file path. Overrides appropriate section file path.\n"
        "  --romfsdir=dir     Specify RomFS directory path. Overrides appropriate section directory path.\n"
        "  --listromfs        List files in RomFS.\n"
        "  --baseromfs        Set Base RomFS to use with update partitions.\n"
        "  --basenca          Set Base NCA to use with update partitions.\n"
        "  --basefake         Use a fake Base RomFS with update partitions (all reads will return 0xCC).\n"
        "  --onlyupdated      Ignore non-updated files in update partitions.\n"
        "  --xcontenttype=    Only extract contents if the content type matches an expected one.\n"
        "                         Supported types are integers 0-9 or [program, meta, control, manual, data, publicdata].\n"
        "  --appendsectypes   Append a section type string to section paths.\n"
        "  --suppresskeys     Suppress output of decrypted keys.\n"
        "NPDM options:\n"
        "  --json=file        Specify file path for saving JSON representation of program permissions to.\n"
        "KIP1 options:\n"
        "  --json=file        Specify file path for saving JSON representation of program permissions to.\n"
        "  --uncompressed=f   Specify file path for saving uncompressed KIP1.\n"
        "NSO0 options:\n"
        "  --uncompressed=f   Specify file path for saving uncompressed NSO0.\n"
        "PFS0 options:\n"
        "  --pfs0dir=dir      Specify PFS0 directory path.\n"
        "  --outdir=dir       Specify PFS0 directory path. Overrides previous path, if present.\n"
        "  --exefsdir=dir     Specify PFS0 directory path. Overrides previous paths, if present for ExeFS PFS0.\n"
        "RomFS options:\n"
        "  --romfsdir=dir     Specify RomFS directory path.\n"
        "  --outdir=dir       Specify RomFS directory path. Overrides previous path, if present.\n"
        "  --listromfs        List files in RomFS.\n"
        "HFS0 options:\n"
        "  --hfs0dir=dir      Specify HFS0 directory path.\n"
        "  --outdir=dir       Specify HFS0 directory path. Overrides previous path, if present.\n"
        "  --exefsdir=dir     Specify HFS0 directory path. Overrides previous paths, if present.\n"
        "XCI options:\n"
        "  --rootdir=dir      Specify XCI root HFS0 directory path.\n"
        "  --updatedir=dir    Specify XCI update HFS0 directory path.\n"
        "  --normaldir=dir    Specify XCI normal HFS0 directory path.\n"
        "  --securedir=dir    Specify XCI secure HFS0 directory path.\n"
        "  --logodir=dir      Specify XCI logo HFS0 directory path.\n"
        "  --outdir=dir       Specify XCI directory path. Overrides previous paths, if present.\n"
        "Package1 options:\n"
        "  --package1dir=dir  Specify Package1 directory path.\n"
        "  --outdir=dir       Specify Package1 directory path. Overrides previous path, if present.\n"
        "Package2 options:\n"
        "  --package2dir=dir  Specify Package2 directory path.\n"
        "  --outdir=dir       Specify Package2 directory path. Overrides previous path, if present.\n"
        "  --extractini1      Enable INI1 extraction to default directory (redundant with --ini1dir set).\n"
        "  --ini1dir=dir      Specify INI1 directory path. Overrides default path, if present.\n"
        "INI1 options:\n"
        "  --ini1dir=dir      Specify INI1 directory path.\n"
        "  --outdir=dir       Specify INI1 directory path. Overrides previous path, if present.\n"
        "  --saveini1json     Enable generation of JSON descriptors for all INI1 members.\n"
        "NAX0 options:\n"
        "  --sdseed=seed      Set console unique seed for SD card NAX0 encryption.\n"
        "  --sdpath=path      Set relative path for NAX0 key derivation (ex: /registered/000000FF/cafebabecafebabecafebabecafebabe.nca).\n"
        "Save data options:\n"
        "  --outdir=dir       Specify save directory path.\n"
        "  --listfiles        List files in save file.\n"
        "Key Derivation options:\n"
        "  --sbk=key          Set console unique Secure Boot Key for key derivation.\n"
        "  --tseckey=key      Set console unique TSEC Key for key derivation.\n"
        "\n";
}

char *interpolate(char *pattern, ...) {
    char *output;

    va_list argptr;
    va_start(argptr, pattern);
    vsnprintf(output, MAX_PATH, pattern, argptr);
    va_end(argptr);

    return output;
}

void addLogLine(hactool_ctx_t *tool_ctx, char *text) {
    cJSON_AddItemToArray(tool_ctx->log, cJSON_CreateString(text));
}

void addWarning(hactool_ctx_t *tool_ctx, char *text) {
    cJSON_AddItemToArray(tool_ctx->warnings, cJSON_CreateString(text));
}