# 2巡目：直通を含む1〜4経路のTCPファイル転送比較

目的は、node3からnode1への同じ総量のデータ転送について、中継ノードの組み合わせと分配量を変え、転送完了までの時間を比較することです。Python 3.6以降の標準ライブラリだけを使います。実験用コンテナ内で実行してください。

これは経路検討用の補助プログラムです。グループの最終実装へ結果を移す際には、その実装でも再測定してください。実験4-1のCプログラムとの速度差には言語・プロトコル実装の差も含まれるため、両者の差をすべて経路の効果とは扱えません。この比較内では同じプログラム・1000バイトのバッファ・同じファイル・同じハッシュ検証方式を使います。

## 比較する8通り

| ケース | 本数 | 使用経路 | 2巡目のリンク容量だけによる合計上限 |
|---|---:|---|---:|
| D | 1 | 3→1 | 10Mbps |
| D2 | 2 | 3→1、3→2→1 | 11Mbps |
| D4 | 2 | 3→1、3→4→1 | 20Mbps |
| D5 | 2 | 3→1、3→5→1 | 11Mbps |
| D24 | 3 | 3→1、3→2→1、3→4→1 | 21Mbps |
| D25 | 3 | 3→1、3→2→1、3→5→1 | 12Mbps |
| D45 | 3 | 3→1、3→4→1、3→5→1 | 21Mbps |
| D245 | 4 | 全4経路 | 22Mbps |

上限は測定結果ではありません。TCP/IPヘッダ、ACK、遅延、立ち上がり、CPU、共有ファイルシステム等を無視した参考値です。対象は直通と中継1ノードまでの経路です。2ノード以上を経由する経路は含みません。

経路ごとの設定上のボトルネックは、直通10Mbps、node2経由1Mbps、node4経由10Mbps、node5経由1Mbpsです。例えば4経路へ均等に25%ずつ割り当てると、1Mbpsの経路の完了を待つため、4本が遅くなる可能性があります。まず10:1:10:1を目安に、実測速度から分配量を決めます。

## プログラムの仕組み

- node3のsourceがファイルを読み、指定された範囲だけをTCPで送ります。
- node2・4・5のrelayが、node3へ要求を渡し、受け取ったデータを直ちにnode1へ転送します。ファイル全体を溜めてから送る方式ではありません。
- node1のclientが各経路を別スレッド・別接続で同時に使います。ファイルを重複しない範囲へ分割し、それぞれ指定した位置へ保存します。
- ハッシュは元データのSHA-256です。各経路が送った範囲を受信側で照合し、不足・破損は失敗として記録します。
- 中継経路ではTCP接続を中継ノードで分けます。OSの経路表、帯域制限、遅延設定は変更しません。
- `elapsed_sec`はメタデータ問い合わせ開始から、全経路の受信・保存・範囲ハッシュ検証とファイルクローズが完了するまでです。全体グッドプットは「元ファイルの転送バイト数×8÷全体時間」で計算します。各経路のMbpsを足した値を全体速度にはしません。
- `--bytes`を付けると、元ファイルの先頭の指定バイト数だけを実際に転送します。省略時は全体です。
- 接続の作成は計測時間に含めます。`time -p`で見るプロセス全体時間には、Pythonの起動とCSV等の結果出力も含まれます。

## 0. 配置

このREADMEと同じ`route_compare`フォルダを、実験室PCの実験環境の`ics-exp-lan/share/route_compare`へコピーしてください。コンテナからは次のように見える必要があります。

```text
/home/ubuntu/share/route_compare/
  routebench.py
  lab_control.py
  endpoints_start2.json
  weights_start2_nominal.json
  README.md
```

`routebench.py`はWindows上で実行するのではなく、実験室の各node上で動かします。`lab_control.py`は実験室PCの物理マシン側からDocker経由で補助プログラムを起動・終了します。

## 1. 起動（物理マシン側）

いつもの`ics-exp-lan`のMakefileがあるディレクトリで実行します。既に正しい2巡目で動いているなら、その状態を使えます。

```bash
make start2
make ps
```

起動エラーがないことを確認してください。違う巡目が動いている場合は、授業の環境切替手順を先に実施してください。比較用プログラム自体は巡目を変更しません。

各nodeのIP・制限設定を記録します。

```bash
python3 share/route_compare/lab_control.py snapshot
```

`environment_start2.txt`が作成されます。実際にstart2の設定であることも確認してください。固定IPが配布構成と違う場合は、`endpoints_start2.json`だけでなく`lab_control.py`内の中継先も実際のリンクに合わせる必要があります。

## 2. 元ファイルを作る（物理マシン側、初回だけ）

512MiBのファイルを作ります。既にこの名前で必要な元ファイルがある場合は、作り直す必要はありません。

```bash
docker exec node3 sh -c 'head -c 536870912 /dev/urandom > /home/ubuntu/share/route_compare/route_test.dat'
docker exec node3 wc -c /home/ubuntu/share/route_compare/route_test.dat
```

表示されるサイズは536870912バイトです。元ファイルと受信ファイルを同じ名前にしないでください。全nodeでshareを共有していますが、転送プログラムは必ずTCPを通してデータを運びます。受信側で共有元ファイルを直接読んで転送を省略する処理はありません。

## 3. sourceとrelayを起動する（物理マシン側）

```bash
python3 share/route_compare/lab_control.py start
python3 share/route_compare/lab_control.py status
```

node3、node2、node4、node5がREADYになれば準備完了です。各nodeの補助プログラムはバックグラウンドで動きます。追加のターミナルは不要です。失敗した場合は`share/route_compare/logs/`の該当nodeのログを確認してください。

使用する接続先は次のとおりです。

| 経路 | node1が接続する場所 | 中継ノードが接続するnode3のIP |
|---|---|---|
| direct | 172.21.0.30:19000 | なし |
| via2 | 172.20.0.20:19001 | 172.24.0.30:19000 |
| via4 | 172.22.0.40:19001 | 172.27.0.30:19000 |
| via5 | 172.23.0.50:19001 | 172.28.0.30:19000 |

受信側のnode1が接続を開始し、ファイルデータはnode3→node1方向へ流れます。

## 4. node1で動作確認

```bash
make login-node1
cd ~/share/route_compare
python3 routebench.py client --paths direct --bytes 1048576 --output results/check_direct.bin
python3 routebench.py client --paths direct,via4 --bytes 1048576 --output results/check_two.bin
```

`SHA256 OK`と表示されることを確認します。ここでの転送量は1MiBです。実測速度が配布設定から大きく外れている場合は、巡目・IP・帯域制限を確認します。

以後の測定コマンドもnode1で実行します。node3等へ毎回ログインし直す必要はありません。

## 5. 各経路を単独測定して分配比を求める

```bash
python3 routebench.py calibrate --bytes 2097152 --repeats 2 --out-dir results/calibration
```

4経路を1本ずつ、2MiBずつ、2回測ります。ここは分配比を得る準備なので、via2のみ等も測定します。ケースD〜D245の本比較では、必ず直通を含めます。

`results/calibration/weights.json`へ単独実測グッドプットの中央値を書き出します。設定値10:1:10:1をそのまま決めつけず、実験室で実際に出た速度を使います。失敗した経路があれば、準備完了とは扱わずエラー終了します。

## 6. 全8通りを少量データで比較する

```bash
python3 routebench.py suite --bytes 20971520 --repeats 3 --allocation weighted --weights-file results/calibration/weights.json --out-dir results/screen
```

20MiBを、8通り×3回転送します。これは正式結果の前の予備測定です。全ケースで同じ総バイト数を転送します。本数を増やしても、総量を2倍・3倍にはしません。各回のケースの順番は入れ替えます。

記録は`results/screen/results.csv`、集計は`summary.json`です。ケースごとに受信ファイルを保存し、同じケースの次の測定で上書きします。CSVにはすべての試行が残ります。

再試行で別実験と区別したい場合は、`results/screen02`等の新しい出力ディレクトリを使ってください。異なる分配方式やファイルサイズのCSVを手作業で混ぜないでください。

## 7. 同時転送時の速度から分配比を補正する

```bash
python3 routebench.py refine results/screen/results.csv --out-dir results/weights_refined
python3 routebench.py suite --bytes 20971520 --repeats 3 --allocation weighted --case-weights-dir results/weights_refined --out-dir results/screen_refined
```

`refine`は、各組み合わせで実際に観測した「経路ごとのバイト数÷その経路の完了時間」を使い、ケース別の分配比を作ります。例えば速い経路は多く、遅い経路は少なく担当します。

これは最適解の保証ではなく、静的な分配比の改善を試す1段階の調整です。元のscreenとscreen_refinedを比べて、調整が改善したか確認してください。差がない／悪化した場合は、元の重みを使う本測定も残します。

## 8. 均等分配を対照条件として確認する

必要なら、4本へ25%ずつ渡した場合も比較できます。

```bash
python3 routebench.py suite --cases D245 --bytes 20971520 --repeats 3 --allocation equal --out-dir results/equal_four
```

これが遅くても、「4経路自体が悪い」と即断しません。分配比の影響を分けて説明します。1Mbpsの経路へ大量に割り当てたために、最後まで残った可能性があります。

## 9. 各本数の候補を大きな同一ファイルで本測定する

まず小さい測定の集計を確認します。

```bash
python3 routebench.py summary results/screen_refined/results.csv
```

その結果から、各本数の最速候補を自動選択し、同じ512MiBで3回ずつ測る場合は次です。

```bash
python3 routebench.py suite --best-from results/screen_refined/results.csv --bytes 536870912 --repeats 3 --allocation weighted --case-weights-dir results/weights_refined --out-dir results/final
```

全候補のうち成功3回以上・失敗なしのものから、中央値が最短の候補を本数ごとに選びます。失敗がある場合は原因を解消してから、別の出力ディレクトリで測定し直します。

候補の差がばらつきと同程度なら、両方を本測定に残してください。例えばD24とD45が近い場合は、次のように明示します。

```bash
python3 routebench.py suite --cases D,D4,D24,D45,D245 --bytes 536870912 --repeats 3 --allocation weighted --case-weights-dir results/weights_refined --out-dir results/final_with_ties
```

上のケース名は例なので、screenの実測結果に合わせます。本測定で短い予備測定と順位が変わることもあります。今回の経路制約と2巡目の名目上限22Mbpsでは、512MiBの転送は理論上でも約195秒かかります。実際にも100秒以上あることを確認してください。

元の単独実測の重みで本測定する場合は、`--case-weights-dir`を外し、`--weights-file results/calibration/weights.json`を使います。どの分配方式を使った結果か、必ず記録してください。

## 10. 結果の読み方

```bash
python3 routebench.py summary results/final/results.csv
```

- `median`：3回の全体時間の中央値。小さい方が速い。
- `range`：最小〜最大時間。候補の差がこの幅に埋もれていないか確認。
- `goodput`：元の総データ量から求めた実効速度。
- `n`：成功回数。`fail`が0であることも確認。
- `per_path`：各経路の担当バイト数と終了までの時間。遅い1経路だけが最後まで残っていないかを見る。

報告表には、1〜4本それぞれの採用経路、分配比、総量、中央値、最小〜最大、Mbps、直通比の速度向上を並べます。同じ総量なら、速度向上倍率は「直通の中央値÷対象の中央値」です。

結論は「2巡目・この総量・このTCP中継実装・この分配方式では、何本が最速だった」と書きます。あらゆる実装での絶対的最速とは断定しません。一般論で4本が速いと決めつけることもありません。

同時に他のグループのiperfや別のファイル転送を同じ自分の仮想環境で走らせず、計測中のパケット全文表示も避けて条件をそろえます。経路確認用の短いtcpdumpは測定前に分けて実行します。

## 終了（物理マシン側）

node1から抜けて、実験環境のMakefileがある場所へ戻ります。

```bash
exit
python3 share/route_compare/lab_control.py stop
make stop
```

`lab_control.py stop`はこの比較用プログラムだけに終了を依頼します。OSの経路表やネットワーク制限を変更することはありません。

## 検証範囲

Windows上のローカルTCPで、全8ケース×均等／重み付きの16通りのデータ一致、分割範囲の欠落・重複がないこと、破損検知、元ファイルの上書き防止、calibrate→suite→refine→本数別候補選択のコマンド連携を確認しています。実験室の帯域・遅延・Docker環境での動作と性能は、実験室で確認する必要があります。

確認コマンド：`python3 -m unittest -v test_routebench`

## 実験室で取得した予備測定

[2巡目・20MiBの集計値と考察](reports/START2_SCREEN_RESULTS.md)を参照してください。画像から転記した集計値であり、元の試行別CSVとは区別しています。
