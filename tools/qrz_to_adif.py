#!/usr/bin/env python3
"""Turns logbook pages saved from QRZ.com into one ADIF file.

    python tools/qrz_to_adif.py oldlogs oldlogs/qrz_logbooks.adi

Each page in the folder is the <tbody> of the logbook table, in a .txt file named after the station that
logged it, with any page number after: 2e0eud3.txt is a page of 2E0EUD's log. A row saved twice is written once.

The page's title attributes are used rather than its text, as they hold the full frequency and comment.
"""
import collections
import glob
import html
import os
import re
import sys

# The page's mode to ADIF's MODE and SUBMODE. DATA isn't a real ADIF mode, but it's what QRZ holds and what kind
# of data isn't known, so it's kept rather than guessed.
MODES = {
    "FT8": ("FT8", ""),
    "FT4": ("MFSK", "FT4"),
    "DATA": ("DATA", ""),
    "MFSK": ("MFSK", ""),
    "PSK31": ("PSK", "PSK31"),
    "VARA HF": ("DYNAMIC", "VARA HF"),
    "CW": ("CW", ""),
    "FM": ("FM", ""),
    "SSB": ("SSB", ""),
    "USB": ("SSB", "USB"),
    "LSB": ("SSB", "LSB"),
}

BANDS = [
    ("160M", 1.8, 2.0), ("80M", 3.5, 4.0), ("60M", 5.06, 5.45), ("40M", 7.0, 7.3), ("30M", 10.1, 10.15),
    ("20M", 14.0, 14.35), ("17M", 18.068, 18.168), ("15M", 21.0, 21.45), ("12M", 24.89, 24.99), ("10M", 28.0, 29.7),
    ("6M", 50.0, 54.0), ("4M", 70.0, 71.0), ("2M", 144.0, 148.0), ("70CM", 420.0, 450.0),
]

# Letters QRZ itself has lost, leaving only a replacement character. Each could only have been the one letter.
LOST_LETTERS = {
    "Jos� Vitor Martins Toste": "José Vitor Martins Toste",
    "Agust�n Serrano": "Agustín Serrano",
    "G�nter Schielke": "Günter Schielke",
}

CELL = re.compile(r'<td[^>]*class="(td_\w+)[^"]*"[^>]*title="([^"]*)"')
MACHINE_COMMENT = re.compile(r"FT\d(?:\s+Sent:\s*(\S+))?(?:\s+Rcvd:\s*(\S+))?")


def undouble(s):
    """QRZ's names are UTF-8 read as Windows-1252 and encoded again: "MÃ¼ller" for "Müller". Undoes that."""
    raw = bytearray()
    for ch in s:
        if ord(ch) < 0x100:
            raw.append(ord(ch))
        else:
            try:
                raw += ch.encode("cp1252")
            except UnicodeEncodeError:
                return s
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return s


def clean(s):
    s = undouble(html.unescape(s))
    s = LOST_LETTERS.get(s.strip(), s)
    return " ".join(s.split())


def band_of(mhz):
    for name, low, high in BANDS:
        if low <= mhz <= high:
            return name
    raise ValueError(f"no band for {mhz} MHz")


def field(name, value):
    return f"<{name}:{len(value.encode('utf-8'))}>{value} " if value else ""


def record(row):
    mode, submode = MODES[row["mode"]]
    out = field("CALL", row["call"])
    out += field("QSO_DATE", row["when"][:10].replace("-", ""))
    out += field("TIME_ON", row["when"][11:13] + row["when"][14:16])
    out += field("FREQ", row["freq"])
    out += field("BAND", band_of(float(row["freq"])))
    out += field("MODE", mode) + field("SUBMODE", submode)
    out += field("RST_SENT", row["sent"]) + field("RST_RCVD", row["rcvd"])
    out += field("NAME", row["name"])
    out += field("GRIDSQUARE", row["grid"])
    out += field("COUNTRY", row["country"])
    out += field("COMMENT", row["comment"])
    if row["lotw_sent"]:
        out += field("LOTW_QSL_SENT", "Y")
    if row["lotw_date"]:
        out += field("LOTW_QSL_RCVD", "Y") + field("LOTW_QSLRDATE", row["lotw_date"].replace("-", ""))
    out += field("STATION_CALLSIGN", row["station"])
    out += field("APP_QRZLOG_LOGID", row["id"])
    out += field("APP_QRZLOG_STATUS", "C" if row["confirmed"] else "N")
    if row["qrz_date"]:
        out += field("APP_QRZLOG_QSLDATE", row["qrz_date"].replace("-", ""))
    return out + "<EOR>\n"


def parse_row(tr, station):
    head = tr[: tr.find(">") + 1]
    cells = {k: clean(v) for k, v in CELL.findall(tr)}
    title = re.search(r"<title>(.*?)</title>", tr, re.S)
    title = html.unescape(title.group(1)).strip() if title else ""
    lotw = re.match(r"Confirmed by LoTW on (\d{4}-\d\d-\d\d)", title)
    qrz = re.match(r"Confirmed on (\d{4}-\d\d-\d\d)", title)
    if not (lotw or qrz or title.startswith("Unconfirmed")):
        raise ValueError(f"unknown status: {title!r}")

    # The comment FT8 logging software writes is only the reports, so they go where reports belong.
    comment, sent, rcvd = cells["td_comments"], "", ""
    machine = MACHINE_COMMENT.fullmatch(comment)
    if machine:
        comment, sent, rcvd = "", machine.group(1) or "", machine.group(2) or ""
    # The log has nowhere for a QRZ confirmation, and ignores the APP_ fields, so it's kept where it can be read.
    if qrz:
        comment = (comment + " " if comment else "") + f"[Confirmed on QRZ {qrz.group(1)}]"

    return {
        "id": re.search(r'data-rownum="(\d+)"', head).group(1),
        "station": station,
        "when": cells["td_date"],
        "call": cells["td_call2"].upper(),
        "freq": cells["td_freq2"],
        "mode": cells["td_mode2"],
        "grid": cells["td_grid2"],
        "country": cells["td_country2"],
        "name": cells["td_name2"],
        "comment": comment,
        "sent": sent,
        "rcvd": rcvd,
        "lotw_sent": "lotw_sent" in head,
        "lotw_date": lotw.group(1) if lotw else "",
        "qrz_date": qrz.group(1) if qrz else "",
        "confirmed": bool(lotw or qrz),
        "lnum": int(re.search(r"td_lnum[^>]*>\s*(\d+)", tr).group(1)),
    }


def fold_data_twins(rows):
    """Some contacts reached QRZ twice: once with the real mode and reports, and again in the same minute as
    DATA, sometimes with the finer grid or the only LoTW confirmation. Folds each DATA row into its twin."""
    real = collections.defaultdict(list)
    for r in rows:
        if r["mode"] != "DATA":
            real[(r["station"], r["call"], band_of(float(r["freq"])), r["when"])].append(r)
    kept, folded = [], 0
    for r in rows:
        twins = real[(r["station"], r["call"], band_of(float(r["freq"])), r["when"])] if r["mode"] == "DATA" else []
        if not twins:
            kept.append(r)
            continue
        t = twins[0]
        folded += 1
        if len(r["grid"]) > len(t["grid"]):
            t["grid"] = r["grid"]
        t["name"] = t["name"] or r["name"]
        t["country"] = t["country"] or r["country"]
        t["lotw_sent"] = t["lotw_sent"] or r["lotw_sent"]
        t["lotw_date"] = t["lotw_date"] or r["lotw_date"]
        t["confirmed"] = t["confirmed"] or r["confirmed"]
        if r["qrz_date"] and not t["qrz_date"]:
            t["qrz_date"] = r["qrz_date"]
            note = f"[Confirmed on QRZ {r['qrz_date']}]"
            t["comment"] = (t["comment"] + " " if t["comment"] else "") + note
        if r["comment"] and r["comment"] not in t["comment"]:
            t["comment"] = (t["comment"] + " " if t["comment"] else "") + r["comment"]
    return kept, folded


def main(folder, out_path):
    rows, repeats = {}, 0
    for path in sorted(glob.glob(os.path.join(folder, "*.txt"))):
        station = re.match(r"[A-Za-z0-9]*?[A-Za-z]+(?=\d*\.txt$)", os.path.basename(path)).group(0).upper()
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
        for tr in re.split(r"(?=<tr\b)", text):
            if not tr.startswith("<tr") or "lrow-nav" in tr[:60]:
                continue
            row = parse_row(tr, station)
            if row["id"] in rows:
                repeats += 1
            rows[row["id"]] = row

    # Which rows of each log were seen, before any are folded together.
    seen = collections.defaultdict(set)
    for r in rows.values():
        seen[r["station"]].add(r["lnum"])
    kept, folded = fold_data_twins(list(rows.values()))
    ordered = sorted(kept, key=lambda r: (r["when"], r["station"], r["call"]))
    stations = collections.Counter(r["station"] for r in ordered)
    header = "QRZ.com logbooks " + ", ".join(sorted(stations)) + "\n"
    header += field("ADIF_VER", "3.1.4") + field("PROGRAMID", "qrz_to_adif") + "<EOH>\n"
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + "".join(record(r) for r in ordered))

    print(f"{len(ordered)} QSOs written to {out_path}, {repeats} repeated rows left out, {folded} DATA twins folded in")
    for station, count in sorted(stations.items()):
        have = sorted(seen[station])
        missing = sorted(set(range(1, have[-1] + 1)) - set(have))
        gap = f", rows {missing[0]}-{missing[-1]} missing ({len(missing)})" if missing else ""
        print(f"  {station}: {count}, from rows {have[0]}-{have[-1]}{gap}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
