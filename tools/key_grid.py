import sys
sys.path.insert(0,'tools')
import key_eval as K
songs=K.load_songs('C:/cb/keycache')
print(len(songs),'songs')
for cols in ['peaks','sal_g','sub','bassall','canc','canc_bass','canc_g','canc+sub','canc_g+sub']:
    for prof in ['ks','temperley','shaath']:
        print('%-11s %-9s'%(cols,prof), K.fmt(K.evaluate(songs, dict(profile=prof, cols=cols))))
