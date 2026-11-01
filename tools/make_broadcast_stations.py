#!/usr/bin/env python3
"""Writes stations/broadcast.json from the EiBi schedule and the FCC's AM list.

Shortwave schedules change at the end of March and October, so run this each season:

  curl -o sked.csv http://www.eibispace.de/dx/sked-b26.csv      (A = summer, B = winter, then the year)
  curl -o README.TXT http://www.eibispace.de/dx/README.TXT
  curl -o fcc.txt "https://transition.fcc.gov/fcc-bin/amq?serv=AM&vac=3&freq=530&fre2=1700&class=A&list=4&NS=N&size=9"
  python tools/make_broadcast_stations.py sked.csv README.TXT fcc.txt B26 2026-11-01 stations/broadcast.json

EiBi allows use "within third-party software". FCC data is US government work.
"""
import collections
import datetime
import re
import sys
import textwrap
import unicodedata

# What EiBi lists that isn't a broadcaster: coast stations, weather, aviation, military, numbers, jammers.
NOT_BROADCAST = re.compile(
    r"Coastguard|Coastgd|USCG|Meteo|Meterolog|Met Fax|\bFax\b|\bWx\b|Volmet|Spy Numbers|Channel Marker|Navy|French Ny|Jammer|Firedrake|"
    r"DIGITAL|Aeradio|Military|Air Force|RTTY|NAVTEX|LDOC|Kystradio|Trading|Buzzer|RFID|Unknown Station|Various|Filler music|Marine R|"
    r"Maritime|Weather Bureau|Rescue|Seychelles Control|Ejercito|\(USB\)|\bUSB\b|NZART|IRTS|WA0RCR|DWD|Hopen Island|Harbor|Mogadishu Radio|"
    r"^(Arrecife|Auckland|Bay Islands|Ben Thuy|Bjornoya|Bodo|Brisbane|Ca Mau|Cabo de Gata|Cabo la Nao|Can Tho|Chennai|Colombo|Coru.a|Da Nang|"
    r"Dakar|Delhi|Duyen Hai|Gander|Hai Phong|Ho Chi Minh|Hon Gai|Hue|Iceland|Kien Giang|Kolkata|Las Palmas|Lyngby|Mong Cai|Mumbai|Nadi|Naha|"
    r"New York|Nha Trang|Olimpia|Phan Rang|Phan Tiet|Phu Yen|Piarco|Quy Nhon|Reykjavik|Romaradio|San Francisco|Santa Maria|Stockholm|Tahiti|"
    r"Tokyo|Valencia|Valentia|Vung Tau) |"
    r"^(CBA|CBM|CBP|CBV|EAO|FUE|FUG|FUO|JFE|KFS|KLB|KLN|KPH|LZW|OSU|PFC|SVO|TAH|VAJ|VCF|VCO|VCP|VCS|VFF|VOJ|VOK|VMC|VMW|WLO|XSG|XSQ|XSV|ZLM|ZSC|"
    r"WO2XPY) ")
# Not schedules at all: an unused alternative, something heard by accident, or one date.
SKIP_DAYS = re.compile(r"^(alt|harm|imod|spur|LSB|USB|\d\.|Last|\d+[A-Z][a-z][a-z]$|alt[A-Z]|Haj|Ram|MF-)")

# The short name for the flag, and what the note calls it. Searching finds either.
NAMES = {
    "China Radio Int.": ("CRI", "China Radio International, part of China Media Group, known abroad as the Voice of China"),
    "China National Radio 1": ("CNR 1", "China National Radio 1, the Voice of China"),
    "Voice of America": ("VOA", "Voice of America"),
    "VoA Radio Ashna": ("VOA Ashna", "Voice of America Radio Ashna"),
    "BBC": ("BBC", "BBC World Service"),
    "Radio France Int.": ("RFI", "Radio France Internationale"),
    "NHK Radio Japan": ("NHK", "NHK World-Radio Japan"),
    "KBS World Radio": ("KBS World", "KBS World Radio"),
    "#KBS World Radio": ("KBS World", "KBS World Radio"),
    "Radio Romania Int.": ("RRI", "Radio Romania International"),
    "Radio Exterior Espana": ("REE", "Radio Exterior de Espana"),
    "Radio Vaticana": ("Vatican R.", "Vatican Radio, Radio Vaticana"),
    "Radio Taiwan Int.": ("RTI", "Radio Taiwan International"),
    "Radio Habana Cuba": ("RHC", "Radio Habana Cuba"),
    "AIR Akashvani Ext.Sce": ("AIR", "All India Radio, Akashvani, External Service"),
    "IRIB Voice of I.R.Iran": ("VOIRI", "IRIB Voice of the Islamic Republic of Iran"),
    "Trans World Radio": ("TWR", "Trans World Radio"),
    "Adventist World Radio": ("AWR", "Adventist World Radio"),
    "Voice of Vietnam": ("VOV", "Voice of Vietnam"),
    "Voice of Turkey": ("VO Turkey", "TRT Voice of Turkey"),
    "R.Algiers Int.": ("R. Algiers", "Radio Algiers International"),
    "WRMI Radio Miami Int.": ("WRMI", "WRMI Radio Miami International"),
    "Radio Slovakia Int.": ("RSI", "Radio Slovakia International"),
    "R.Argentina al Exterior": ("RAE", "Radio Argentina al Exterior"),
}


def ascii_text(s):
    """The flags' font and the search are simplest with plain letters: Espana is what most people type."""
    s = s.translate(str.maketrans({"ø": "o", "Ø": "O", "æ": "ae", "ß": "ss", "ł": "l", "đ": "d"}))
    return unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()


COUNTRIES = {
    "China (People's Republic)": "China", "Korea, North (Democratic People's Republic)": "North Korea", "Korea, South (Republic)": "South Korea",
    "United Kingdom of Great Britain and Northern Ireland": "United Kingdom", "United States of America": "United States",
    "Russian Federation": "Russia", "Brasil": "Brazil", "Tchad": "Chad", "The Netherlands": "Netherlands", "Swaziland": "Eswatini",
    "Vatican State": "Vatican", "Guam / Guahan": "Guam", "Hong Kong, part of China": "Hong Kong",
}


def country_name(text):
    text = re.sub(r"\s*\(also .*?\)|\s*\*$", "", text).strip()
    return COUNTRIES.get(text, text)


def short_label(name):
    """A flag has to fit between its neighbours. The note keeps the whole name."""
    label = re.sub(r"\s+", " ", name.lstrip("#")).strip()
    label = re.sub(r"^(?:China National Radio|CNR) ?(\d+).*", r"CNR \1", label)
    label = re.sub(r"^Voice of Vietnam (\d)", r"VOV \1", label)
    label = re.sub(r"^(WWCR|CFRX)\b.*", r"\1", label)
    label = re.sub(r"^([KW][A-Z]{3}) .*|^([KW][A-Z]{2}) .*, [A-Z]{2}\b.*", r"\1\2", label)  # a US call sign says enough
    label = label.split(",")[0].strip()
    return label.replace("Radio ", "R. ") if len(label) > 14 else label


def read_codes(path):
    with open(path, encoding="cp1252", errors="replace") as f:
        lines = f.read().splitlines()
    at = {}
    for i, line in enumerate(lines):
        for key, title in (("lang", "I) Language codes."), ("country", "II) Country codes."), ("target", "III) Target-area codes."), ("site", "IV) Transmitter site codes.")):
            if line.strip() == title and i > 100:
                at[key] = i
    langs, countries, targets, sites = {}, {}, {}, collections.defaultdict(dict)
    for line in lines[at["lang"]:at["country"]]:
        m = re.match(r"\s+([A-Z0-9-]{1,4})\s{2,}(.+)$", line)
        if m:
            langs[m.group(1)] = re.split(r"[:(\[]", m.group(2))[0].strip()
    for line in lines[at["country"]:at["target"]]:
        m = re.match(r"\s+([A-Z0-9]{1,3})\*?\s{2,}(.+)$", line)
        if m:
            countries[m.group(1)] = country_name(ascii_text(m.group(2)))
    for line in lines[at["target"]:at["site"]]:
        m = re.match(r"\s+([A-Za-z.]{2,3})\s+- (.+)$", line)
        if m:
            targets[m.group(1)] = re.sub(r"\s*\(.*\)$", "", m.group(2)).strip()
    current = None
    for line in lines[at["site"]:]:
        m = re.match(r"\s+([A-Z0-9]{1,3}):\s+(.*)$", line)
        if m:
            current = m.group(1)
            text = m.group(2)
        elif current and line.startswith("     "):
            text = line.strip()
        else:
            continue
        m = re.match(r"([A-Za-z0-9]{1,3})-(.+)$", text)
        if not m:
            continue
        place = re.split(r"\s+\d{1,2}[NS]\d|\s+\d{2}[NS]", m.group(2))[0]
        place = re.sub(r"\(.*?\)|\".*?\"|\s*\d+\s*kW", "", place).strip(" ,;")
        if place:
            sites[current][m.group(1)] = ascii_text(place)
    return langs, countries, targets, sites


def target_name(code, targets, countries):
    if not code:
        return ""
    if code in targets:
        return targets[code]
    for prefix, word in (("C", "Central"), ("E", "East"), ("N", "North"), ("S", "South"), ("W", "West")):
        if code.startswith(prefix) and code[1:] in targets:
            return word + " " + targets[code[1:]]
    return countries.get(code, code)


def site_name(code, home, sites, countries):
    code = code.strip()
    if not code:
        return ""
    if code.startswith("/"):
        m = re.match(r"/([A-Z0-9]{1,3})(?:-(\w+))?", code)
        if not m:
            return ""
        country = countries.get(m.group(1), m.group(1))
        place = sites.get(m.group(1), {}).get(m.group(2) or "")
        return f"{place}, {country}" if place else country
    return sites.get(home, {}).get(code.lstrip("-"), "")


DAY = {"Mo": 1, "Tu": 2, "We": 3, "Th": 4, "Fr": 5, "Sa": 6, "Su": 7}


def parse_days(text):
    """The days as digits, 1 for Monday, or "" for every day or a word such as irr."""
    if re.fullmatch(r"[1-7]+", text):
        return "".join(sorted(text))
    m = re.fullmatch(r"(Mo|Tu|We|Th|Fr|Sa|Su)-(Mo|Tu|We|Th|Fr|Sa|Su)", text)
    if m:
        a, b = DAY[m.group(1)], DAY[m.group(2)]
        days = [a]
        while days[-1] != b:
            days.append(days[-1] % 7 + 1)
        return "".join(str(d) for d in sorted(days))
    words = re.findall(r"Mo|Tu|We|Th|Fr|Sa|Su", text)
    return "".join(str(d) for d in sorted(DAY[w] for w in words)) if words else ""


def day_words(digits):
    names = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
    return ",".join(names[int(d) - 1] for d in digits)


def ended(stop, today):
    """A stop date is ddmm, in a season that runs from the last days of one month into the next year."""
    m = re.match(r"(\d\d)(\d\d)", stop)
    if not m:
        return False
    day, month = int(m.group(1)), int(m.group(2))
    if not 1 <= month <= 12 or not 1 <= day <= 31:
        return False
    # Without a year, anything further back than the season's six months is next year's.
    year = today.year
    try:
        when = datetime.date(year, month, day)
    except ValueError:
        return False
    if (today - when).days > 190:
        when = datetime.date(year + 1, month, day)
    return when < today


def json_text(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def khz_text(khz):
    return f"{khz:g}"


def main(sked, readme, fcc, season, today_text, out):
    today = datetime.date.fromisoformat(today_text)
    langs, countries, targets, sites = read_codes(readme)
    # (country, kHz, flag) -> what's on it
    entries = collections.defaultdict(lambda: {"names": None, "rows": []})
    with open(sked, encoding="cp1252", errors="replace") as f:
        lines = f.read().splitlines()[1:]
    for line in lines:
        r = line.split(";")
        if len(r) < 11:
            continue
        khz = float(r[0])
        raw = ascii_text(r[4]).strip()
        if khz < 148.5 or r[5].startswith("-") or r[8] == "8" or NOT_BROADCAST.search(raw) or SKIP_DAYS.match(r[2]):
            continue
        if r[8] == "6" and ended(r[10], today):
            continue
        if r[1][:4] == r[1][5:9]:  # a typing slip in the source: it lasts no time at all
            continue
        label, full = NAMES.get(raw, (short_label(raw), raw))
        country = countries.get(r[3], r[3] or "Elsewhere")
        e = entries[(country, khz, label)]
        e["names"] = full
        e["rows"].append((r[1], parse_days(r[2]), "irregular" if r[2] == "irr" else "", r[5], r[6], site_name(r[7], r[3], sites, countries)))

    # US clear-channel AM, the ones that carry across a continent at night. Only US rows are the FCC's own
    # licences: its foreign rows are old international notifications that list stations long since closed.
    usa = countries.get("USA", "United States")
    seen = collections.defaultdict(set)
    with open(fcc, encoding="latin-1") as f:
        for line in f:
            c = [x.strip() for x in line.strip().strip("|").split("|")]
            if len(c) < 14 or c[11] != "US" or c[8] != "LIC":
                continue
            seen[(float(c[1].split()[0]), c[0], c[9].title(), c[10])].add(float(c[13].split()[0]))
    for (khz, call, city, state), kw in seen.items():
        call = call.split("-")[0]
        if not any(k[1] == khz and k[2].startswith(call) for k in entries):
            e = entries[(usa, khz, call)]
            e["names"] = f"{call}, {city}, {state}. Class A clear channel, {max(kw):g} kW"
            e["fcc"] = True

    groups = collections.defaultdict(list)
    for (country, khz, label), e in entries.items():
        spans, notes = [], []
        # Runs that follow on, such as 1400-1500 and 1500-2200, read as one.
        merged = {}
        for times, days, word, lang, target, site in sorted(e["rows"], key=lambda x: x[0]):
            key = (days, word, lang, target, site)
            start, stop = times.split("-")
            runs = merged.setdefault(key, [])
            if runs and runs[-1][1] == start:
                runs[-1][1] = stop
            else:
                runs.append([start, stop])
        # One line for each language, target and site, with all its times.
        lines = collections.defaultdict(list)
        for (days, word, lang, target, site), runs in merged.items():
            for run in runs:
                if run[0] == run[1]:  # two runs that join up all the way round the clock
                    run[0], run[1] = "0000", "2400"
                start, stop = run
                spans.append(f"{start}-{stop}" + (" " + days if days else ""))
                lines[(lang, target, site)].append(" ".join(x for x in (f"{start}-{stop}", day_words(days), word) if x))
        for (lang, target, site), times in lines.items():
            extra = [langs.get(lang, lang)] if lang else []
            to = target_name(target, targets, countries)
            if to:
                extra.append("to " + to)
            if site:
                extra.append("from " + site)
            text = ", ".join(sorted(set(times))) + " UTC" + (": " + ", ".join(extra) if extra else "")
            notes.append("\n".join(textwrap.wrap(text, 90)))
        note = e["names"] + (f". {country}" if not e.get("fcc") else "")
        if notes:
            note += "\n" + "\n".join(sorted(set(notes)))
        groups[country].append((khz, label, note, sorted(set(spans))))

    parts = []
    total = 0
    for country in sorted(groups):
        rows = []
        for khz, label, note, spans in sorted(groups[country], key=lambda x: (x[0], x[1])):
            total += 1
            on = ', "on": [' + ", ".join(json_text(s) for s in spans) + "]" if spans else ""
            rows.append(f'        {{"khz": {khz_text(khz)}, "label": {json_text(label)}, "note": {json_text(note)}{on}}}')
        parts.append(f'    {{\n      "service": "broadcast", "name": {json_text(country)}, "mode": "AM",\n      "stations": [\n' + ",\n".join(rows) + "\n      ]\n    }")
    source = (f"EiBi shortwave and medium wave schedule, season {season}, by Eike Bierwirth, eibispace.de (free for use in third-party software); "
              f"FCC AM Query, licensed US Class A stations, {today.strftime('%B %Y')}. Long wave is in the Worldwide stations list.")
    note = ("Broadcasters by home country, with the times they're scheduled in UTC. Schedules change at the end of March and October. "
            "Listed isn't heard: reception depends on the hour, the season and where a station aims its signal, and stations change plans at short notice. "
            "Local medium wave stations outside the US clear channels aren't listed, unless they also reach abroad.")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("{\n"
                '  "name": "Worldwide stations: broadcasters",\n'
                f'  "revision": {json_text(today.strftime("%Y-%m") + " (" + season + ")")},\n'
                f'  "source": {json_text(source)},\n'
                f'  "note": {json_text(note)},\n'
                '  "groups": [\n' + ",\n".join(parts) + "\n  ]\n}\n")
    print(f"{total} stations in {len(groups)} countries")


if __name__ == "__main__":
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    main(*sys.argv[1:])
