#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
routeplan.py -- リンクの調査結果から転送経路を決め，各ノードの起動スクリプトを作る

  $ python3 routeplan.py                       # 複数経路（最大流）で計画
  $ python3 routeplan.py --mode single         # 最良の単一経路だけ使う
  $ python3 routeplan.py --path 3-4-5-1        # 経路を手で指定（比較実験用）
  $ python3 routeplan.py --links links_tc2.txt # 調査結果のファイルを指定

入力: links/node*.txt（collect_links.sh の出力）．1 行 1 リンク（送信方向）
      "送信元 宛先 帯域Mbps [遅延ms] [インタフェース]"
出力: 画面に経路の候補・最大流・割り当てを表示し，plan/ に起動スクリプトを作る
      plan/node3.sh … サーバ（fserver.out）
      plan/node2,4,5.sh … 中継（relay.out）
      plan/node1.sh … クライアント（fclient.out）
      plan/stop.sh  … 全プロセスの停止（各ノードで実行）

アルゴリズム:
  単一経路: すべての単純経路を列挙し，ボトルネック帯域が最大のもの
            （同じなら遅延の合計が小さいもの，ホップ数が少ないもの）を選ぶ
  複数経路: Edmonds-Karp 法で最大流を求め，流量を「流せる量が最大の経路」から順に
            経路へ分解する．流量が --min-mbps 未満の経路は使わない
            （遅い経路にチャンクが残ると最後に待たされるため）
"""
import argparse
import glob
import json
import math
import shlex
import os
import sys
from collections import deque

SERVER_PORT = 10000
RELAY_PORT_BASE = 20000

# 2 ノード間のリンクのサブネット（docker-compose.yml の lanA-B: 172.SUB.0.0/16）
SUBNET = {(1, 2): 20, (1, 3): 21, (1, 4): 22, (1, 5): 23, (2, 3): 24,
          (2, 4): 25, (2, 5): 26, (3, 4): 27, (3, 5): 28, (4, 5): 29}


def ip_of(node, peer):
    """node が peer とのリンク上で持つ IP アドレス"""
    sub = SUBNET[(min(node, peer), max(node, peer))]
    return "172.%d.0.%d0" % (sub, node)


def load_links(files):
    cap, delay = {}, {}
    for fn in files:
        with open(fn) as f:
            for line in f:
                line = line.split("#")[0].split()
                if len(line) < 3:
                    continue
                s, d, mbps = int(line[0]), int(line[1]), float(line[2])
                cap[(s, d)] = mbps
                delay[(s, d)] = float(line[3]) if len(line) > 3 else 0.0
    return cap, delay


def simple_paths(nodes, cap, s, d, max_hops):
    out = []

    def dfs(path):
        u = path[-1]
        if u == d:
            out.append(list(path))
            return
        if len(path) - 1 >= max_hops:
            return
        for v in nodes:
            if v not in path and cap.get((u, v), 0) > 0:
                path.append(v)
                dfs(path)
                path.pop()
    dfs([s])
    return out


def path_stats(p, cap, delay):
    links = list(zip(p, p[1:]))
    return (min(cap[l] for l in links), sum(delay[l] for l in links), len(links))


def max_flow(nodes, cap, s, d):
    """Edmonds-Karp．flow[(u,v)] を返す"""
    flow = {}

    def residual(u, v):
        return cap.get((u, v), 0) - flow.get((u, v), 0) + flow.get((v, u), 0)

    total = 0.0
    while True:
        prev = {s: None}
        q = deque([s])
        while q and d not in prev:
            u = q.popleft()
            for v in nodes:
                if v not in prev and residual(u, v) > 1e-9:
                    prev[v] = u
                    q.append(v)
        if d not in prev:
            break
        aug, v = float("inf"), d
        while prev[v] is not None:
            aug = min(aug, residual(prev[v], v))
            v = prev[v]
        v = d
        while prev[v] is not None:
            u = prev[v]
            back = min(flow.get((v, u), 0), aug)       # 逆向きの流れを先に打ち消す
            flow[(v, u)] = flow.get((v, u), 0) - back
            flow[(u, v)] = flow.get((u, v), 0) + aug - back
            v = u
        total += aug
    return total, {k: x for k, x in flow.items() if x > 1e-9}


def decompose(nodes, flow, s, d, delay):
    """流量を経路に分解する（流せる量が多い経路から）"""
    flow = dict(flow)
    paths = []
    while True:
        best = None
        for p in simple_paths(nodes, flow, s, d, len(nodes)):
            links = list(zip(p, p[1:]))
            amt = min(flow[l] for l in links)
            key = (amt, -sum(delay.get(l, 0) for l in links), -len(links))
            if best is None or key > best[0]:
                best = (key, p, amt)
        if best is None or best[2] <= 1e-9:
            break
        _, p, amt = best
        for l in zip(p, p[1:]):
            flow[l] -= amt
            if flow[l] <= 1e-9:
                del flow[l]
        paths.append((p, amt))
    return paths


def rtt_ms(p, delay):
    """経路の往復遅延 [ms]（データの向き＋要求/ACK の逆向き）"""
    links = list(zip(p, p[1:]))
    return sum(delay.get(l, 0) for l in links) + sum(delay.get((v, u), 0) for u, v in links)


def depth_for(mbps, rtt, args):
    """往復時間の間に流れるデータ量（BDP）の 1.5 倍を先出しできる要求数"""
    need = mbps * 1e6 / 8 * (rtt / 1000.0) * 1.5
    return max(args.depth, min(64, int(-(-need // (args.chunk * 1024)))))


def fmt_path(p):
    return "→".join(str(x) for x in p)


def write_scripts(outdir, src, dst, chosen, standby, args, delay, maxcap):
    """argv形式のplanと、このplanだけを管理する起動・終了スクリプトを生成する。"""
    os.makedirs(outdir, exist_ok=True)
    for fn in glob.glob(os.path.join(outdir, "node*.sh")):
        os.remove(fn)
    per_node, services, client_targets = {}, {}, []
    root = os.path.dirname(os.path.abspath(__file__))
    relative_root = os.path.relpath(root, outdir)
    header = ('#!/bin/bash\nset -euo pipefail\n'
              'plan_dir=$(cd -- "$(dirname -- "$0")" && pwd)\n'
              'cd -- "$plan_dir"/' + shlex.quote(relative_root) + '\n')

    services.setdefault(str(src), []).append({
        "command": ["fserver.out", args.file, str(args.server_port)], "ready": "fserver:"})
    entries = [(p, mbps, False) for p, mbps in chosen] + [(p, 0.0, True) for p in standby]
    for k, (p, mbps, is_standby) in enumerate(entries, 1):
        port = args.relay_port_base + k
        for i, node in enumerate(p[1:-1]):
            prev_node = p[i]
            next_port = args.server_port if prev_node == src else port
            services.setdefault(str(node), []).append({
                "command": ["relay.out", str(port), ip_of(prev_node, node), str(next_port)],
                "ready": "relay:"})
        last_hop = p[-2]
        target_port = args.server_port if last_hop == src else port
        target = "%s:%d" % (ip_of(last_hop, dst), target_port)
        if args.conns > 1 and not is_standby:
            target += "x%d" % args.conns
        rate = maxcap if is_standby else mbps / args.conns
        dep = depth_for(rate, rtt_ms(p, delay), args)
        if dep != args.depth:
            target += "/d%d" % dep
        if not is_standby:
            target += "@%g" % mbps
        target += "=" + "-".join(str(x) for x in p)
        client_targets.append((target, p, mbps, is_standby))

    client = ["./fclient.out", "-o", args.output, "-c", str(args.chunk), "-d", str(args.depth),
              "-m", str(args.min_mbps)]
    for target, _, _, standby_path in client_targets:
        if standby_path:
            client += ["-s", target]
    client += [target for target, _, _, standby_path in client_targets if not standby_path]
    cmd = " ".join(shlex.quote(value) for value in client)
    manifest = {"version": 1, "source": src, "destination": dst, "services": services,
                "client": client, "chosen": chosen, "standby": standby,
                "nominal_mbps": sum(rate for _, rate in chosen)}
    with open(os.path.join(outdir, "plan.json"), "w") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)

    for node in sorted({int(n) for n in services} | {dst}):
        body = ''
        if str(node) in services:
            body += 'python3 services.py start "$plan_dir/plan.json" --node %d\n' % node
        if node == dst:
            body += 'time -p ' + cmd + ' "$@"\n'
        per_node[node] = body
        fn = os.path.join(outdir, "node%d.sh" % node)
        with open(fn, "w") as stream:
            stream.write(header + body)
        os.chmod(fn, 0o755)
    stop = os.path.join(outdir, "stop.sh")
    with open(stop, "w") as stream:
        stream.write(header + 'python3 services.py stop "$plan_dir/plan.json"\n')
    os.chmod(stop, 0o755)
    return per_node, cmd


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="経路の決定と起動スクリプトの生成")
    ap.add_argument("--links", nargs="*", help="リンク表（既定: links/node*.txt）")
    ap.add_argument("--src", type=int, default=3, help="送信元ノード（サーバ）")
    ap.add_argument("--dst", type=int, default=1, help="宛先ノード（クライアント）")
    ap.add_argument("--mode", choices=["multi", "single"], default="multi")
    ap.add_argument("--path", help="経路を手で指定（例 3-4-5-1．複数ならカンマ区切り）")
    ap.add_argument("--min-mbps", type=float, default=5.0,
                    help="この流量未満の経路は使わない（既定 5）")
    ap.add_argument("--max-hops", type=int, default=4, help="候補の最大ホップ数")
    ap.add_argument("--conns", type=int, default=1, help="1 経路あたりの TCP 接続数")
    ap.add_argument("--chunk", type=int, default=256, help="チャンクサイズ KiB")
    ap.add_argument("--depth", type=int, default=4, help="1 接続あたりの先出し要求数")
    ap.add_argument("--file", default="test.dat", help="送信するファイル（サーバ側）")
    ap.add_argument("--output", default="received.dat", help="保存先（クライアント側）")
    ap.add_argument("--size-mb", type=float, default=100.0, help="所要時間の見積もり用")
    ap.add_argument("--standby-hops", type=int, default=3,
                    help="この数以下のホップの経路を予備経路にする（0 で予備なし）")
    ap.add_argument("--server-port", type=int, default=SERVER_PORT)
    ap.add_argument("--relay-port-base", type=int, default=RELAY_PORT_BASE)
    ap.add_argument("--outdir", default=os.path.join(here, "plan"))
    args = ap.parse_args()

    if (not 1 <= args.server_port <= 65535 or not 1 <= args.relay_port_base <= 65500
            or not 1 <= args.conns <= 64 or not 1 <= args.chunk <= 65536
            or not 1 <= args.depth <= 64 or not math.isfinite(args.min_mbps)
            or args.min_mbps < 0 or args.standby_hops < 0):
        ap.error("invalid port, connection, chunk, depth, or threshold")

    files = args.links or sorted(glob.glob(os.path.join(here, "links", "node*.txt")))
    if not files:
        sys.exit("リンク表がありません．各ノードで ./collect_links.sh を実行するか --links を指定してください")
    cap, delay = load_links(files)
    nodes = sorted({n for l in cap for n in l})
    s, d = args.src, args.dst

    print("== リンクの帯域 [Mbps]（行: 送信元, 列: 宛先） 入力: %s" % ", ".join(os.path.basename(f) for f in files))
    print("      " + "".join("%8s" % ("n%d" % v) for v in nodes))
    for u in nodes:
        print("  n%d  " % u + "".join("%8s" % ("—" if u == v else ("%g" % cap[(u, v)] if (u, v) in cap else "?"))
                                       for v in nodes))

    cands = simple_paths(nodes, cap, s, d, args.max_hops)
    if not cands:
        ap.error("no path connects source and destination")
    cands.sort(key=lambda p: (-path_stats(p, cap, delay)[0],) + path_stats(p, cap, delay)[1:])
    print("\n== 単一経路の候補 node%d → node%d（ボトルネック帯域の大きい順，上位 8）" % (s, d))
    for p in cands[:8]:
        bw, dl, h = path_stats(p, cap, delay)
        print("  %-12s %6.1f Mbps  片道遅延 %5.1f ms  %d ホップ  100MB %6.1f 秒" % (fmt_path(p), bw, dl, h, 800 / bw))

    total, flow = max_flow(nodes, cap, s, d)
    decomp = decompose(nodes, flow, s, d, delay)
    print("\n== 最大流: %.1f Mbps" % total)
    for p, amt in decomp:
        mark = "" if amt >= args.min_mbps else "  （--min-mbps 未満のため使わない）"
        print("  %-12s %6.1f Mbps%s" % (fmt_path(p), amt, mark))

    if args.path:
        chosen = []
        for spec in args.path.split(","):
            p = [int(x) for x in spec.split("-")]
            if len(p) < 2 or len(set(p)) != len(p) or p[0] != s or p[-1] != d or any(l not in cap for l in zip(p, p[1:])):
                sys.exit("経路 %s は使えません" % spec)
            chosen.append((p, path_stats(p, cap, delay)[0]))
        why = "手で指定"
    elif args.mode == "single":
        chosen = [(cands[0], path_stats(cands[0], cap, delay)[0])]
        why = "最良の単一経路"
    else:
        chosen = [(p, amt) for p, amt in decomp if amt >= args.min_mbps]
        why = "最大流の分解（%.1f Mbps 以上の経路）" % args.min_mbps
    if not chosen:
        ap.error("no path meets --min-mbps")
    rate = sum(m for _, m in chosen)

    print("\n== 採用する経路（%s）: 合計 %.1f Mbps → %.0f MB は約 %.1f 秒" % (why, rate, args.size_mb, args.size_mb * 8 / rate))
    for k, (p, m) in enumerate(chosen, 1):
        r = rtt_ms(p, delay)
        print("  経路%d %-12s %6.1f Mbps  RTT %5.1f ms  先出し %d 個" % (k, fmt_path(p), m, r, depth_for(m / max(1, args.conns), r, args)))

    # 予備経路：使わない経路も中継を用意し，転送中に速度を測って速ければ使う
    # （ネットワークの設定が途中で変わっても，速くなった経路に乗り換えられる）
    used = [p for p, _ in chosen]
    standby = [p for p in simple_paths(nodes, cap, s, d, args.standby_hops) if p not in used]
    standby.sort(key=lambda p: (len(p), p))
    if standby:
        print("\n== 予備経路（%d 本．転送中にプローブして %.1f Mbps 以上出れば使い始める）"
              % (len(standby), args.min_mbps))
        print("  " + "  ".join("%s(%g)" % (fmt_path(p), path_stats(p, cap, delay)[0]) for p in standby))
    maxcap = max(cap.values())
    per_node, cmd = write_scripts(args.outdir, s, d, chosen, standby, args, delay, maxcap)
    print("\n== 実行手順（共有フォルダの %s に生成）" % os.path.relpath(args.outdir, os.getcwd()))
    for node in sorted(per_node, key=lambda n: (n == d, n)):
        role = "サーバ" if node == s else ("クライアント（最後に実行）" if node == d else "中継")
        print("  node%d: ./plan/node%d.sh   # %s" % (node, node, role))
    print("  終了後: 各ノードで ./plan/stop.sh")
    print("\n  クライアントのコマンド: %s" % cmd)


if __name__ == "__main__":
    main()
