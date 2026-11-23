# Converts anchor_ai_model.pkl (made by train_anchor_ai.py) into anchor_model.npz,
# which anchor_laptop_test.py reads. Needs: numpy, scikit-learn.  Run after every retrain:
#   python make_model_npz.py ../anchor_ai_model.pkl anchor_model.npz
import pickle, sys
import numpy as np
src = sys.argv[1] if len(sys.argv) > 1 else "anchor_ai_model.pkl"
dst = sys.argv[2] if len(sys.argv) > 2 else "anchor_model.npz"
clf = pickle.load(open(src, "rb"))
feat, thr, left, right, start, cls = [], [], [], [], [], []
for rnd in clf._predictors:
    for c, tp in enumerate(rnd):
        start.append(len(feat)); cls.append(c)
        for nd in tp.nodes:
            if nd["is_leaf"]:
                feat.append(-1); thr.append(float(nd["value"])); left.append(0); right.append(0)
            else:
                feat.append(int(nd["feature_idx"])); thr.append(float(nd["num_threshold"]))
                left.append(int(nd["left"])); right.append(int(nd["right"]))
np.savez_compressed(dst, base=np.asarray(clf._baseline_prediction, dtype=np.float64).ravel(),
                    start=np.array(start, np.int64), cls=np.array(cls, np.int64),
                    feat=np.array(feat, np.int64), thr=np.array(thr, np.float32),
                    left=np.array(left, np.int64), right=np.array(right, np.int64))
print("wrote", dst, "-", len(start), "trees,", len(feat), "nodes")
