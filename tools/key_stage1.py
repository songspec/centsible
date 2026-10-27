import sys, itertools
import numpy as np
sys.path.insert(0,'tools')
import key_eval as K
songs=[s for s in K.load_songs('C:/cb/keycache') if not s.edge]
def fam_acc(cfg, prof='temperley', mode='best24'):
    P=K.profile_matrix(K.PROFILES[prof]); ok=0
    for s in songs:
        ch=K.chroma_of(s,cfg['cols'],cfg.get('weight',0.5),norm=cfg.get('norm','l2'),power=cfg.get('power',1.0))
        sc=K.score_pearson(ch,P)
        if mode=='best24':
            k=int(np.argmax(sc)); fam=k if k<12 else (k-12+3)%12
        else:                              # best family by logsumexp of its major and relative-minor scores
            fs=[np.logaddexp(8*sc[f],8*sc[12+(f+9)%12]) for f in range(12)]; fam=int(np.argmax(fs))
        ok+= fam in s.fams
    return ok/len(songs)
print('n',len(songs))
for cols in ['peaks','peaks_g','mid','sal','canc']:
    for w,pw,nm in itertools.product([0,0.5,1.0],[1.0,0.5],['l2','sum']):
        a=fam_acc(dict(cols=cols,weight=w,power=pw,norm=nm))
        b=fam_acc(dict(cols=cols,weight=w,power=pw,norm=nm),mode='lse')
        print('%-8s weight %.1f power %.1f norm %-3s  family temperley best24 %.3f  lse %.3f'%(cols,w,pw,nm,a,b))
