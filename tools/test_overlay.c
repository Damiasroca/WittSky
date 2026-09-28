#include <stdio.h>
#include <string.h>

#include "overlay_eco_parse.h"
#include "overlay_units.h"

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (cond)
        return;
    fprintf(stderr, "FAIL %s\n", msg);
    g_fail++;
}

static void test_units(void)
{
    int32_t milli = 0, mms = 0, mc = 0;
    char unit[16];
    char buf[32];

    expect(ov_parse_milli("3.2 m/s", &milli, unit, sizeof unit) == 0, "parse 3.2");
    expect(milli == 3200, "3.2 milli");
    expect(strcmp(unit, "m/s") == 0, "unit m/s");
    expect(ov_wind_to_mms(milli, unit, &mms) == 0 && mms == 3200, "m/s to mm/s");

    expect(ov_parse_milli("4.68 km/h", &milli, unit, sizeof unit) == 0, "parse km/h");
    expect(ov_wind_to_mms(milli, unit, &mms) == 0 && mms == 1300, "km/h to mm/s");
    expect(ov_wind_tenths(1300, 0) == 47, "4.7 km/h tenths");
    expect(ov_wind_tenths(1300, 1) == 13, "1.3 m/s tenths");
    expect(ov_wind_tenths(1000, 2) == 22, "1 m/s in mph tenths");
    expect(ov_wind_tenths(1000, 3) == 19, "1 m/s in kn tenths");

    expect(ov_parse_milli("79.2", &milli, unit, sizeof unit) == 0, "parse 79.2");
    expect(ov_temp_to_mc(milli, "F", &mc) == 0 && mc == 26222, "79.2 F");
    expect(ov_temp_to_mc(27000, "\xe2\x84\x83", &mc) == 0 && mc == 27000, "celsius sign");
    expect(ov_wind_to_mms(11200, "km/h", &mms) == 0 && mms == 3111, "11.2 km/h");
    expect(ov_temp_tenths(0, 1) == 320, "0 C is 32.0 F");
    expect(ov_temp_tenths(100000, 1) == 2120, "100 C is 212.0 F");
    expect(ov_temp_tenths(-5500, 0) == -55, "-5.5 C");
    expect(ov_temp_tenths(27200, 0) == 272, "27.2 C");

    expect(strcmp(ov_card16(0), "N") == 0, "N");
    expect(strcmp(ov_card16(180), "S") == 0, "S");
    expect(strcmp(ov_card16(90), "E") == 0, "E");
    expect(strcmp(ov_card16(45), "NE") == 0, "NE");
    expect(strcmp(ov_card16(359), "N") == 0, "359 N");
    expect(strcmp(ov_card16(207), "SSW") == 0, "207 SSW");

    ov_fmt_wind(buf, sizeof buf, 6389, 180, 0);
    expect(strcmp(buf, "23.0 km/h S") == 0, buf);
    ov_fmt_temp(buf, sizeof buf, 27200, 0);
    expect(strcmp(buf, "27.2\xC2\xBA" "C") == 0, buf);

    expect(ov_wind_to_mms(1000, "furlongs", &mms) != 0, "bad wind unit");
    expect(ov_parse_milli("nope", &milli, unit, sizeof unit) != 0, "bad number");
}

static void test_parse(const char *json, int want, int32_t mms, int32_t mc,
                       int dir, int rain, const char *name)
{
    ov_sample_t s;
    const char *why = NULL;
    int rc = ov_eco_parse(json, &s, &why);
    if (want != 0) {
        expect(rc != 0, name);
        return;
    }
    if (rc != 0) {
        fprintf(stderr, "FAIL %s: %s\n", name, why ? why : "?");
        g_fail++;
        return;
    }
    expect(s.wind_mms == mms, name);
    expect(s.temp_mc == mc, name);
    expect(s.dir_deg == dir, name);
    expect(s.raining == rain, name);
    expect(s.ok, name);
}

int main(void)
{
    test_units();
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"6.3\",\"unit\":\"C\"},"
        "{\"id\":\"0x0B\",\"val\":\"3.2 m/s\"},"
        "{\"id\":\"0x0A\",\"val\":\"207\"}"
        "],\"rain\":[{\"id\":\"0x0E\",\"val\":\"0.0 mm/Hr\"}]}",
        0, 3200, 6300, 207, 0, "blog sample");
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"79.2\",\"unit\":\"F\"},"
        "{\"id\":\"0x0b\",\"val\":\"4.68 km/h\"},"
        "{\"id\":\"0x0A\",\"val\":\"180\"}"
        "],\"rain\":[{\"id\":\"0x0E\",\"val\":\"1.2 mm/Hr\"}]}",
        0, 1300, 26222, 180, 1, "km/h F rain");
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"10\",\"unit\":\"C\"},"
        "{\"id\":\"0x0B\",\"val\":\"1.0\",\"unit\":\"m/s\"},"
        "{\"id\":\"0x0A\",\"val\":\"360\"}"
        "],\"piezoRain\":[{\"id\":\"0x0E\",\"val\":\"0.5 in/Hr\"}]}",
        0, 1000, 10000, 0, 1, "piezo rate dir 360");
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"10\",\"unit\":\"C\"},"
        "{\"id\":\"0x0B\",\"val\":\"0\",\"unit\":\"m/s\"},"
        "{\"id\":\"0x0A\",\"val\":\"0\"}"
        "],\"rain\":[{\"id\":\"0x0E\",\"val\":\"0.0 mm/Hr\"}],"
        "\"piezoRain\":[{\"id\":\"srain_piezo\",\"val\":\"1\"}]}",
        0, 0, 10000, 0, 1, "srain flag");
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"10\",\"unit\":\"C\"},"
        "{\"id\":\"0x0A\",\"val\":\"10\"}]}",
        -1, 0, 0, 0, 0, "missing wind");
    test_parse(
        "{\"code\":0,\"msg\":\"success\",\"data\":{"
        "\"outdoor\":{\"temperature\":{\"unit\":\"\xe2\x84\x83\",\"value\":\"27.0\"}},"
        "\"wind\":{\"wind_speed\":{\"unit\":\"km/h\",\"value\":\"11.2\"},"
        "\"wind_direction\":{\"unit\":\"\xc2\xba\",\"value\":\"176\"}},"
        "\"rainfall\":{\"rain_rate\":{\"unit\":\"mm/hr\",\"value\":\"0.0\"}},"
        "\"rainfall_piezo\":{\"rain_rate\":{\"unit\":\"mm/hr\",\"value\":\"0.0\"},"
        "\"state\":{\"value\":\"0\"}}}}",
        0, 3111, 27000, 176, 0, "cloud sample");
    test_parse(
        "{\"code\":0,\"data\":{"
        "\"outdoor\":{\"temperature\":{\"unit\":\"\xe2\x84\x83\",\"value\":\"10\"}},"
        "\"wind\":{\"wind_speed\":{\"unit\":\"km/h\",\"value\":\"0\"},"
        "\"wind_direction\":{\"value\":\"90\"}},"
        "\"rainfall_piezo\":{\"rain_rate\":{\"value\":\"1.5\"},\"state\":{\"value\":\"1\"}}}}",
        0, 0, 10000, 90, 1, "cloud piezo rain");
    test_parse("{\"code\":-1,\"msg\":\"fail\",\"data\":{}}", -1, 0, 0, 0, 0, "cloud code");
    test_parse("{", -1, 0, 0, 0, 0, "truncated");
    test_parse(
        "{\"common_list\":["
        "{\"id\":\"0x02\",\"val\":\"10\",\"unit\":\"C\"},"
        "{\"id\":\"0x0B\",\"val\":\"1\",\"unit\":\"m/s\"},"
        "{\"id\":\"0x0A\",\"val\":\"10\"}"
        "],\"rain\":[{\"id\":\"0x0E\",\"val\":\"fast\"}]}",
        -1, 0, 0, 0, 0, "bad rain");
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
