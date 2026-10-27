"""Minimal .xlsx reader (stdlib only): yields rows of each sheet as lists of strings."""
import zipfile, re, xml.etree.ElementTree as ET
NS = {'m': 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'}
def col_index(ref):
    letters = re.match(r'[A-Z]+', ref).group(0)
    n = 0
    for ch in letters: n = n * 26 + ord(ch) - 64
    return n - 1
def read(path):
    z = zipfile.ZipFile(path)
    shared = []
    if 'xl/sharedStrings.xml' in z.namelist():
        root = ET.fromstring(z.read('xl/sharedStrings.xml'))
        for si in root.findall('m:si', NS):
            shared.append(''.join(t.text or '' for t in si.iter('{%s}t' % NS['m'])))
    wb = ET.fromstring(z.read('xl/workbook.xml'))
    rels = ET.fromstring(z.read('xl/_rels/workbook.xml.rels'))
    relmap = {r.get('Id'): r.get('Target') for r in rels}
    sheets = {}
    for s in wb.find('m:sheets', NS):
        rid = s.get('{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id')
        target = relmap[rid]; target = target if target.startswith('xl/') else 'xl/' + target.lstrip('/')
        root = ET.fromstring(z.read(target))
        rows = []
        for row in root.iter('{%s}row' % NS['m']):
            vals = {}
            for c in row.findall('m:c', NS):
                v = c.find('m:v', NS); t = c.get('t')
                if t == 's' and v is not None: val = shared[int(v.text)]
                elif t == 'inlineStr': val = ''.join(x.text or '' for x in c.iter('{%s}t' % NS['m']))
                else: val = v.text if v is not None else ''
                vals[col_index(c.get('r'))] = val
            if vals:
                rows.append([vals.get(i, '') for i in range(max(vals) + 1)])
        sheets[s.get('name')] = rows
    return sheets
if __name__ == '__main__':
    import sys
    for name, rows in read(sys.argv[1]).items():
        print('SHEET', name, len(rows), 'rows')
        for r in rows[:8]: print(r)
