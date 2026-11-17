#!/usr/bin/env python3
"""Public Suffix List -> rust_lib/src/fetch/public_suffix_list.txt

Usage: python3 tools/rbrowse/psl2txt.py public_suffix_list.dat rust_lib/src/fetch/public_suffix_list.txt

Get the list from https://publicsuffix.org/list/public_suffix_list.dat (MPL 2.0,
see rust_lib/src/fetch/public_suffix_list.NOTICE). The output holds one rule per
line ("com", "*.ck", "!www.ck"), lowercase ASCII, sorted by byte value without
duplicates: cookies.rs binary-searches the text as is. Unicode rules are
converted label by label to their IDNA ("xn--") form, which is how such hosts
reach rbrowse (its URL parser takes ASCII host names only).
"""
import sys

def ascii_rule(rule):
    prefix = ''
    if rule.startswith('!'):
        prefix, rule = '!', rule[1:]
    labels = []
    for label in rule.split('.'):
        if label == '*' or label.isascii():
            labels.append(label.lower())
        else:
            labels.append(label.encode('idna').decode('ascii').lower())
    return prefix + '.'.join(labels)

rules = set()
failed = []
for line in open(sys.argv[1], encoding='utf-8'):
    rule = line.split()[0] if line.split() else ''
    if not rule or rule.startswith('//'):
        continue
    try:
        rules.add(ascii_rule(rule))
    except UnicodeError:
        failed.append(rule)
if failed:
    sys.exit('cannot convert to IDNA: ' + ' '.join(failed))
with open(sys.argv[2], 'w', encoding='ascii', newline='\n') as out:
    for rule in sorted(rules, key=lambda r: r.encode()):
        out.write(rule + '\n')
print(len(rules), 'rules')
