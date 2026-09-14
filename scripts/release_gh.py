# -*- coding: utf-8 -*-
"""release_gh.py — GitHub Release 长期自动发布（gh CLI 路线，2026-09-14）

取代 git credential fill + 手写 REST（GCM 弹窗依赖 + TLS 不稳重试复杂）:
gh auth login 一次授权后 token 由 gh 加密保管（keyring），长期有效。
走本机系统代理（localhost:15236，代理地址有变时改 HTTPS_PROXY）。

用法:
  python scripts\\release_gh.py v2026.09.14-2 [asset_path]

  asset 缺省 = installer\\dist\\weasel-llm-setup-<tag 去掉 v>.exe
  notes 用 --notes-file 时从 stdin/参数给；本脚本约定 notes 存
  installer\\dist\\release_notes_<tag>.md（无则生成骨架并暂停让编辑）。

流程: 查重（tag 已有 release 则 upload --clobber 换资产）→ create →
上传 → API 回读核对字节数。
"""
import os
import subprocess
import sys
import time

GH = r"C:\Users\Administrator\bin\gh.exe"
REPO = "zhanghaozhecn/rime-llm-ime"
os.environ["HTTPS_PROXY"] = "http://localhost:15236"  # 本机系统代理


def run(args, input_text=None, tries=5):
    for i in range(tries):
        p = subprocess.run([GH] + args, capture_output=True, text=True,
                           input=input_text, timeout=300,
                           cwd=os.path.dirname(os.path.dirname(
                               os.path.abspath(__file__))))
        if p.returncode == 0:
            return p.stdout
        print("retry %d: rc=%d %s" % (i + 1, p.returncode,
                                      p.stderr.strip()[:200]))
        time.sleep(4 * (i + 1))
    print("ERROR: gh failed: %s" % " ".join(args[:3]))
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    tag = sys.argv[1]
    repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asset = (sys.argv[2] if len(sys.argv) > 2 else
             os.path.join(repo_dir, "installer", "dist",
                          "weasel-llm-setup-%s.exe" % tag.lstrip("v")))
    if not os.path.exists(asset):
        print("ERROR: asset not found: %s" % asset)
        sys.exit(2)
    notes = os.path.join(repo_dir, "installer", "dist",
                         "release_notes_%s.md" % tag.lstrip("v"))
    if not os.path.exists(notes):
        with open(notes, "w", encoding="utf-8") as f:
            f.write("## %s\n\n(编辑发布说明后重跑本脚本)\n" % tag)
        print("notes skeleton created: %s — edit then rerun" % notes)
        sys.exit(2)

    # 查重: tag 的 release 已存在则换资产, 否则创建
    exists = subprocess.run(
        [GH, "release", "view", tag, "-R", REPO, "--json", "id"],
        capture_output=True, text=True, timeout=60).returncode == 0
    if exists:
        print("release exists, uploading --clobber")
        run(["release", "upload", tag, asset, "--clobber", "-R", REPO])
    else:
        run(["release", "create", tag, "--title", tag,
             "--notes-file", notes, asset, "-R", REPO])

    # 回读核对
    out = run(["release", "view", tag, "-R", REPO, "--json", "assets"])
    import json
    name = os.path.basename(asset)
    size = os.path.getsize(asset)
    for a in json.loads(out)["assets"]:
        if a["name"] == name:
            print("readback: local=%d remote=%d %s" % (
                size, a["size"], "MATCH" if a["size"] == size else "MISMATCH"))
            sys.exit(0 if a["size"] == size else 1)
    print("ERROR: asset not in release")
    sys.exit(1)


if __name__ == "__main__":
    main()
