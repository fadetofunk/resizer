"""
Generates Resizer.ico: a large screen shrinking to a small screen,
connected by a funnel/arrow to show video compression/resizing.
"""
from PIL import Image, ImageDraw
import math

def draw_icon(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size

    # Color palette
    bg       = (30,  34,  44,  255)   # dark slate
    screen_l = (58,  130, 247, 255)   # blue  (large screen fill)
    screen_s = (72,  200, 140, 255)   # green (small screen fill)
    frame_l  = (120, 170, 255, 255)   # light blue border
    frame_s  = (140, 230, 180, 255)   # light green border
    arrow_c  = (255, 200,  60, 255)   # amber arrow
    glare    = (255, 255, 255,  60)   # subtle glare

    # Background rounded rect
    r = max(2, s // 8)
    d.rounded_rectangle([0, 0, s-1, s-1], radius=r, fill=bg)

    # ── Large screen (left) ──────────────────────────────────────────
    # occupies roughly left 42% of canvas, vertically centred
    lx0 = round(s * 0.04)
    ly0 = round(s * 0.18)
    lx1 = round(s * 0.44)
    ly1 = round(s * 0.72)
    bw  = max(1, s // 32)           # border width

    d.rounded_rectangle([lx0, ly0, lx1, ly1],
                        radius=max(1, s//24), fill=screen_l)
    d.rounded_rectangle([lx0, ly0, lx1, ly1],
                        radius=max(1, s//24), outline=frame_l, width=bw)
    # glare stripe top-left
    d.rectangle([lx0+bw+1, ly0+bw+1, lx0+bw+max(1,(lx1-lx0)//3), ly0+bw+max(1,(ly1-ly0)//5)],
                fill=glare)
    # "play" triangle
    cx = (lx0 + lx1) // 2
    cy = (ly0 + ly1) // 2
    pt = max(2, s // 10)
    d.polygon([cx - pt//2, cy - pt,
               cx - pt//2, cy + pt,
               cx + pt,    cy],     fill=(255, 255, 255, 180))

    # stand / base
    base_w = max(2, (lx1 - lx0) // 3)
    base_h = max(1, s // 24)
    bx0 = (lx0 + lx1) // 2 - base_w // 2
    bx1 = bx0 + base_w
    d.rectangle([bx0, ly1, bx1, ly1 + base_h], fill=frame_l)
    d.rectangle([bx0 - base_w//4, ly1 + base_h,
                 bx1 + base_w//4, ly1 + base_h + max(1, s//32)], fill=frame_l)

    # ── Arrow (centre) ───────────────────────────────────────────────
    ax0 = lx1 + max(1, s // 24)
    ax1 = round(s * 0.62)
    ay  = (ly0 + ly1) // 2
    aw  = max(1, s // 20)          # shaft thickness

    # shaft
    d.rectangle([ax0, ay - aw, ax1, ay + aw], fill=arrow_c)
    # arrowhead
    ah = max(3, s // 8)
    d.polygon([ax1,      ay - ah//2,
               ax1 + ah, ay,
               ax1,      ay + ah//2], fill=arrow_c)

    # ── Small screen (right) ─────────────────────────────────────────
    # visually smaller: ~55 % the height of the large screen
    sh = round((ly1 - ly0) * 0.55)
    sw = round((lx1 - lx0) * 0.55)
    sx0 = ax1 + ah + max(1, s // 24)
    sy0 = ay - sh // 2
    sx1 = sx0 + sw
    sy1 = sy0 + sh
    # clamp right edge
    if sx1 > s - round(s * 0.03):
        overflow = sx1 - (s - round(s * 0.03))
        sx0 -= overflow; sx1 -= overflow

    d.rounded_rectangle([sx0, sy0, sx1, sy1],
                        radius=max(1, s//32), fill=screen_s)
    d.rounded_rectangle([sx0, sy0, sx1, sy1],
                        radius=max(1, s//32), outline=frame_s, width=bw)
    # glare
    d.rectangle([sx0+bw+1, sy0+bw+1,
                 sx0+bw+max(1,(sx1-sx0)//3), sy0+bw+max(1,(sy1-sy0)//5)],
                fill=glare)
    # tiny play triangle
    cx2 = (sx0 + sx1) // 2
    cy2 = (sy0 + sy1) // 2
    pt2 = max(1, s // 18)
    d.polygon([cx2 - pt2//2, cy2 - pt2,
               cx2 - pt2//2, cy2 + pt2,
               cx2 + pt2,    cy2],  fill=(255, 255, 255, 180))

    return img


sizes  = [256, 64, 48, 32, 16]
frames = [draw_icon(sz) for sz in sizes]

# Save as .ico with all sizes embedded
out = r"Resizer\Resizer.ico"
frames[0].save(out, format="ICO",
               sizes=[(sz, sz) for sz in sizes],
               append_images=frames[1:])
print(f"Saved {out}")
