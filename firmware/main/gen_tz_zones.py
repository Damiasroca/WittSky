#!/usr/bin/env python3
import csv
import io
import pathlib
import urllib.request

url = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv"
raw = urllib.request.urlopen(url, timeout=30).read().decode("utf-8")
rows = []
for name, posix in csv.reader(io.StringIO(raw)):
    name = name.strip()
    posix = posix.strip()
    if not name or not posix:
        continue
    if len(name) > 39 or len(posix) > 63:
        print("skip", name, len(name), len(posix))
        continue
    rows.append((name, posix))
rows.sort(key=lambda x: x[0].lower())

out = pathlib.Path(__file__).with_name("tz_zones.c")
lines = [
    "/* IANA -> POSIX TZ. Generated from nayarsystems/posix_tz_db (tzdb rules). */",
    "",
    "#include \"cJSON.h\"",
    "",
    "#include <stdlib.h>",
    "#include <string.h>",
    "",
    "#include \"hp10_bringup.h\"",
    "",
    "typedef struct {",
    "    const char *iana;",
    "    const char *posix;",
    "} tz_ent_t;",
    "",
    "static const tz_ent_t s_tz[] = {",
]
for n, p in rows:
    ns = n.replace("\\", "\\\\").replace('"', '\\"')
    ps = p.replace("\\", "\\\\").replace('"', '\\"')
    lines.append(f'    {{"{ns}", "{ps}"}},')
lines += [
    "};",
    "",
    "static int tz_cmp(const void *key, const void *ent)",
    "{",
    "    return strcasecmp((const char *)key, ((const tz_ent_t *)ent)->iana);",
    "}",
    "",
    "const char *hp10_tz_posix(const char *iana)",
    "{",
    "    if (!iana || !iana[0])",
    "        return NULL;",
    "    const tz_ent_t *e = bsearch(iana, s_tz, sizeof s_tz / sizeof s_tz[0],",
    "                                sizeof s_tz[0], tz_cmp);",
    "    return e ? e->posix : NULL;",
    "}",
    "",
    "void hp10_tz_names_json(struct cJSON *arr)",
    "{",
    "    if (!arr)",
    "        return;",
    "    for (size_t i = 0; i < sizeof s_tz / sizeof s_tz[0]; i++)",
    "        cJSON_AddItemToArray(arr, cJSON_CreateString(s_tz[i].iana));",
    "}",
    "",
]
out.write_text("\n".join(lines), encoding="utf-8")
print("wrote", out, "zones", len(rows), "bytes", out.stat().st_size)
