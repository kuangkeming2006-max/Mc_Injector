import urllib.request
import re

url = 'https://raw.githubusercontent.com/bspkrs/MCPMappingViewer/master/mcp_map_viewer/mcp/versions/1.8.9/joined.srg'
req = urllib.request.Request(url)
resp = urllib.request.urlopen(req)
content = resp.read().decode('utf-8')

fields = {
    'func_96441_U': 'getScoreboard',
    'func_96539_a': 'getObjectiveInDisplaySlot',
    'func_96509_i': 'getPlayersTeam',
    'func_96534_i': 'getSortedScores',
    'func_96653_e': 'getPlayerName',
    'func_96667_a': 'formatPlayerName',
    'field_71071_by': 'inventoryField',
    'field_70460_b': 'armorInventoryField',
    'func_77973_b': 'getItem',
    'func_82816_b_': 'hasColor',
    'func_82814_b': 'getColor'
}

for line in content.splitlines():
    if line.startswith('FD: '):
        parts = line.split()
        notch = parts[1].split('/')[-1]
        srg = parts[2].split('/')[-1]
        if srg in fields:
            print(f'{fields[srg]} = {notch}')
    elif line.startswith('MD: '):
        parts = line.split()
        notch = parts[1].split('/')[-1]
        srg = parts[3].split('/')[-1]
        if srg in fields:
            print(f'{fields[srg]} = {notch}')
