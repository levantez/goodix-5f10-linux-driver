"""SIFT-сопоставление отпечатков (подход SIGFM: локальные признаки + гомография).

Штатный NBIS/bozorth на маленькой площадке (56x176) работает плохо, поэтому используем
SIFT-дескрипторы (как SIGFM в goodixtls-форке).
"""
import numpy as np


def features(img):
    import cv2
    sift = cv2.SIFT_create(nfeatures=0, contrastThreshold=0.02, edgeThreshold=12)
    kp, desc = sift.detectAndCompute(img, None)
    pts = np.array([k.pt for k in kp], dtype=np.float32) if kp else np.zeros((0, 2), np.float32)
    return pts, desc


def match_detail(ptsA, descA, ptsB, descB):
    """Вернуть (инлайеры RANSAC, число good-совпадений по ratio-тесту)."""
    import cv2
    if descA is None or descB is None or len(descA) < 8 or len(descB) < 8:
        return 0, 0
    bf = cv2.BFMatcher(cv2.NORM_L2)
    raw = bf.knnMatch(descA, descB, k=2)
    good = [m for m, n in (p for p in raw if len(p) == 2) if m.distance < 0.8 * n.distance]
    if len(good) < 6:
        return 0, len(good)
    src = np.float32([ptsA[m.queryIdx] for m in good]).reshape(-1, 1, 2)
    dst = np.float32([ptsB[m.trainIdx] for m in good]).reshape(-1, 1, 2)
    # частичное аффинное преобразование устойчивее гомографии на плоском отпечатке
    M, mask = cv2.estimateAffinePartial2D(src, dst, method=cv2.RANSAC,
                                          ransacReprojThreshold=8.0)
    inl = int(mask.sum()) if mask is not None else 0
    return inl, len(good)


def match_score(ptsA, descA, ptsB, descB):
    return match_detail(ptsA, descA, ptsB, descB)[0]


# порог принятия (инлайеров). Калибруется по разрыву genuine/impostor.
ACCEPT_INLIERS = 8
