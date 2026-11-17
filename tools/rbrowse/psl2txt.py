#!/usr/bin/env python3
"""Public Suffix List -> rust_lib/src/fetch/public_suffix_list.txt

Usage: python3 tools/rbrowse/psl2txt.py public_suffix_list.dat rust_lib/src/fetch/public_suffix_list.txt

Get the list from https://publicsuffix.org/list/public_suffix_list.dat (MPL 2.0,
see rust_lib/src/fetch/public_suffix_list.NOTICE). The output holds one rule per
line ("com", "*.ck", "!www.ck"), lowercase ASCII only (rbrowse's URL parser
accepts ASCII host names only, so the Unicode rules could never match), sorted
by byte value without duplicates: cookies.rs binary-searches the text as is.
"""
import sys

rules = set()
for line in open(sys.argv[1], encoding='utf-8'):
    rule = line.split()[0] if line.split() else ''
    if not rule or rule.startswith('//'):
        continue
    if not rule.isascii():
        continue
    rules.add(rule.lower())
with open(sys.argv[2], 'w', encoding='ascii', newline='\n') as out:
    for rule in sorted(rules, key=lambda r: r.encode()):
        out.write(rule + '\n')
print(len(rules), 'rules')
