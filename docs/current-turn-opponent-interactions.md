# 自分のターン中に発生する相手情報・相手選択の監査

## 対象範囲

この監査は、登録済みカード ID 1--1267 と現在のルールエンジンを対象に、
root プレイヤーのターン開始から、ターン終了効果とポケモンチェックが終わるまでを扱う。
次の相手ターンの `TurnStart`、ドロー、通常のカード使用、ワザは探索しない。

項目は次の三群に分ける。

1. **自分のデッキ依存**: 効果源を自分が使用する。直接の効果源がデッキになく、
   ワザ・効果をコピーする手段もなければ、そのデッキの探索から除外できる。
2. **相手のデッキ依存**: 相手の場・どうぐなどから反応的に発生する。相手カードが
   公開されていない段階で、未知の相手デッキを推測して先回り列挙してはならない。
3. **ルール依存**: デッキリストだけでは除外できず、現在の公開局面から到達性を判定する。

カードをコピーする効果、相手のカードを使う効果、カード名を変更する効果が追加された場合は、
単純なカード ID の積集合だけでは除外せず、効果到達グラフも確認する。

## A. 自分のデッキ依存

以下のカードが自分のデッキおよび到達可能なコピー元にない場合、その行の処理は探索から
完全に外せる。ID は将来別デッキを監査するための安定した照合キーである。

| 相手側で必要になる処理 | 効果源カード |
| --- | --- |
| 相手手札を見る、公開する、または内容を参照する | `64 Hoothoot`, `212 Rotom`, `220 Espurr`, `279 Lillie’s Ribombee`, `291 N’s Purrloin`, `542 Mandibuzz`, `551 Pidove`, `561 Whimsicott ex`, `687 Mega Absol ex`, `852 Beautifly`, `954 Luxray ex`, `1149 Energy Swatter`, `1186 Eri` |
| 相手手札からランダムなカードを選ぶ | `103 Snorunt`, `246 Espeon ex`, `376 Rotom`, `433 Team Rocket's Chingling`, `470 Team Rocket's Meowth`, `609 Liepard`, `753 Houndstone`, `843 Aipom`, `895 Scraggy`, `896 Mega Scrafty ex`, `984 Mandibuzz ex`, `1024 Talonflame`, `1076 Furfrou`, `1131 Team Rocket's Bother-Bot` |
| 相手が自分の手札からカードを選ぶ | `273 Banette`, `473 Team Rocket's Porygon`, `538 Sandile`, `539 Krokorok`, `540 Krookodile`, `1087 Hand Trimmer`, `1197 Xerosic’s Machinations` |
| 相手手札を戻して引き直す | `596 Gothitelle`, `1019 Vivillon`, `1080 Unfair Stamp`, `1103 Meddling Memo`, `1213 Judge`, `1217 Team Rocket's Archer`, `1223 Harlequin`, `1237 Lucian` |
| 相手にドローさせる | `98 Chandelure`, `164 Comfey`, `866 Frosmoth` |
| 相手の非公開山札を捨てる、見る、並べ替える、またはそのカード ID を必要とする | `30 Magcargo ex`, `58 Great Tusk`, `198 Durant ex`, `227 Deino`, `228 Zweilous`, `229 Hydreigon ex`, `290 Tyranitar`, `386 Cornerstone Mask Ogerpon`, `435 Team Rocket's Dottler`, `440 Team Rocket's Larvitar`, `471 Team Rocket's Persian ex`, `529 Gurdurr`, `595 Gothorita`, `688 Spiritomb`, `781 Mega Heracross ex`, `824 Flygon`, `881 Team Rocket's Diglett`, `896 Mega Scrafty ex`, `1049 Hippowdon`, `1054 Tyrantrum`, `1091 Accompanying Flute` |
| 相手の裏向きサイドの内容を参照する | `399 Team Rocket's Blipbug`, `1131 Team Rocket's Bother-Bot`, `1226 Lt. Surge's Bargain` |
| 相手が新しいバトルポケモンを選ぶ交代効果 | `22 Hippopotas`, `339 Yanma`, `478 Squawkabilly`, `563 Sawsbuck`, `576 Samurott`, `629 Herdier`, `709 Bayleef`, `899 Pawniard`, `1143 Repel` |

この群でも、相手が選ぶ行為は Chance ではない。相手が観測できる情報状態ごとの Min decision
として扱う。ランダム選択とドローだけが整数重み付き Chance である。

## B. 相手のデッキ依存

次は相手の公開カードが自分の攻撃・きぜつなどへ反応して発生する。未知の相手デッキに
入っている可能性だけを理由に、全カードを探索へ混ぜない。現在の公開盤面に効果源があるか、
そのターン中に公開領域へ出ることが証明された場合だけ有効化する。

| 反応 | 相手側の効果源カード |
| --- | --- |
| 被ダメージ・きぜつ時の yes/no | `416 Huntail`, `824 Flygon` |
| 相手自身の山札検索 | `461 Team Rocket's Koffing`, `898 Pecharunt`, `1169 Amulet of Hope` |
| 相手自身のドロー | `1156 Lucky Helmet` |
| 相手がエネルギーまたは付け先を選ぶ | `1027 Turtonator`, `1160 Heavy Baton`, `1161 Handheld Fan` |
| 1回目の攻撃後に相手が新しいバトルポケモンを選び、攻撃が続く | `93 Dipplin`, `100 Goldeen`, `240 Seaking`, `247 Swirlix` の Festival Lead |

公開済みの効果源と公開対象だけで完結する選択は、その場で厳密に処理できる。相手自身の
山札内容が必要なのに、相手デッキ prior が与えられていない場合は、確率分布そのものが
定義できない。この場合は推測せず `certified=false` とし、
`UnknownOpponentListForReactiveEffect` を理由として返す。

検証用に相手デッキを渡す場合も、それは world 列挙用の環境 prior であり、root プレイヤーの
知識、合法手、評価特徴には加えない。

## C. ルールおよび公開局面依存

- 相手のバトルポケモンがいなくなった場合、相手が公開ベンチから新しいバトルポケモンを選ぶ。
- 自分のポケモンが自分のターン中またはポケモンチェックできぜつした場合、相手がサイドを取る。
  相手にとっても交換可能な未知の裏向き位置だけは商集合化し、得るカード ID を厳密な Chance
  としてbeliefへ反映する。既知位置、表向きサイド、非対称な制約があれば相手Decisionを残す。
- 同時に複数の相手側ダメージ・きぜつ時効果が待機した場合、ルールが相手に順序を選ばせる。
  後続状態が同じ順序だけを再帰前に統合する。
- Area Zero Underdepths が場を離れるなどして相手のベンチ上限を超えた場合、相手が残す
  ポケモンを選ぶ。スタジアムとベンチは公開情報なので、相手デッキの推測は不要である。
- ポケモンチェックのコイン、どく、やけどなどは公開状態から発生する Chance である。
  その結果のきぜつは、サイド取得と新しいバトルポケモン選択へ接続する。
- 前の相手ターンから残っている公開継続効果・使用制限は、効果源が自分のデッキにないことを
  理由に削除してはならない。現在局面の公開状態で到達性を判定する。

物理カード serial、同名コピー、交換可能な裏向きサイド位置、意味のないシャッフル順は
列挙しない。カード ID の異なる結果、相手が捨てるカード集合、新しいバトルポケモン、
検索対象、意味のある山札順、付け先、非可換な効果順、yes/no は統合しない。

## デッキ監査手順

新しい固定デッキでは次の順に絞る。

1. デッキのカード ID と A の効果源 ID の積集合を取る。
2. 残ったカードから、コピー・再利用を含む効果到達グラフを作る。
3. B は相手デッキリストとの積集合ではなく、root 局面の公開盤面から有効化する。
   closed-world 回帰だけは、相手固定デッキとの積集合を事前の到達性証明に使える。
4. C は現在局面の公開フラグ、特殊状態、待機効果、ベンチ数から判定する。
5. 非公開カード ID が本当に必要になった地点だけ、該当領域の枚数ベクトルを展開する。
   関係しない山札・手札・サイドの分割は遅延し、周辺化する。

## `deck/majkel1337-85795098` の監査結果

固定デッキは提出物に同梱した manifest
`sample_submission/sample_submission/exact_solver/decks/majkel1337-85795098.json`（SHA-256
`3f4515092dc59df397f365a9b79c7cf0c1cb73b9aa38bc47c1b18e9df4c2fdaf`）を正とする。
現在の作業ブランチにある別の `deck.csv` はこの判定に使わない。

### 自分のデッキ依存の積集合

A との直接の積集合は **`1197 Xerosic’s Machinations` 3枚だけ**である。
このデッキにはワザ・効果をコピーして A の別カードへ到達する手段もない。

- Xerosic を使った時だけ、相手の実際の手札multisetごとに「3枚になるまで捨てる」選択が必要。
- `1182 Boss’s Orders` は相手ベンチを対象にするが、選択者は自分で対象は公開情報。
- `1081 Enhanced Hammer` も選択者は自分で、相手の場の特殊エネルギーは公開情報。
- このほかの Hilda、Dawn、Poké Pad、Telepath Psychic Energy などは自分の非公開領域だけを扱う。

固定デッキ同士の closed-world 自己対戦では、B の効果源との積集合は空である。
したがって Koffing、Pecharunt、Amulet of Hope などによる相手山札検索は、この回帰では
生成してはならない。

### 今回必要な対策

現実装ではカードID固有のbootstrap方策を削除した。Xerosicの相手手札multisetを多変量
超幾何分布で全列挙し、各手札情報集合で合法な捨て札multisetを全列挙してMinを取る。
同じ公開捨て札へ到達する継続探索は再利用し、未処理質量は厳密な上下限として保持する。
短いsliceで未完でも、処理済み情報集合に暫定方策は混ざらない。

closed-worldの単一root actionを監査するため、任意の相手固定デッキを受け取る
`ExactEvaluateActionV2`も追加した。これはclosed-worldテスト用であり、本番で未知の相手デッキを
補完するAPIではない。

#### P0: Xerosic の相手手札情報集合 Min（実装済み）

1. 効果実行後の pending から `player=opponent`, `zone=hand`, `intent=ConcreteCards` を
   不変な問い合わせオブジェクトへコピーする。効果実行前の parent から player を推測しない。
2. 相手の手札が3枚以下なら分岐せず終了する。
3. 4枚以上なら、相手手札だけをカード ID 別枚数ベクトルで全列挙する。手札枚数を `H`、
   未公開プールを `n_i` とすると重みは `product(C(n_i,h_i))`、総質量は `C(N,H)` である。
4. この時点で不要な相手サイドと山札の分割は列挙しない。手札の補集合を残余beliefとして
   保持する。使用後に到達可能な非Supporter効果が相手手札identityを読む場合は、この圧縮を
   禁止して完全belief経路へ戻す。
5. 同じ相手観測（手札multisetと相手自身の知識）内では、捨てる枚数ベクトルを全列挙し、
   root 評価を最小にするものを選ぶ。捨てる順番と同名物理コピーは一つにする。
6. 公開された捨て札と残余beliefが同じ後続を統合し、Chance総質量の一致を検査する。
7. closed-world では相手固定デッキを列挙 prior に使う。本番で相手 prior がなければ、
   Xerosic を含む root action を即座に `searchStatus="blocked"` とし、同じrootを時間切れまで
   再試行しない。他のroot actionの探索は維持する。

#### P1: きぜつ後の相手バトルポケモン選択

Alakazam、Fezandipiti ex などで相手バトルポケモンをきぜつさせられるため、通常ルールの
相手選択は残す。候補は公開ベンチの `PokemonEntity` で表し、同型Entityだけをまとめる。
固定V3評価器に対して相手の非公開手札が値へ入らないことを依存性検査で証明できる場合は、
相手手札worldを作らず、公開候補に対する Min 一回へ正確に縮約できる。

#### P2: 公開対象の自分選択

Boss’s Orders と Enhanced Hammer は通常の自分Decisionとして扱う。相手beliefを展開せず、
同じcanonical successorになる同型ベンチ対象・同名エネルギー対象を再帰前に統合する。

#### P3: 今回は生成しない経路

固定デッキ同士では、相手の反応的山札検索、反応的ドロー、Festival Lead、相手のエネルギー
移動、Area Zeroによるベンチ縮小、特殊状態によるポケモンチェックきぜつは到達不能である。
これらは汎用コードと監査表には残すが、seed 6 の固定回帰DAGへノードを生成しない。

別の相手デッキを使う実戦では、B と C は公開局面から再判定する。特に Area Zero が公開中に
Nighttime Mine を出す場合は相手のベンチ整理が発生し得るため、固定デッキ自身に Area Zero が
ないという理由だけでは除外しない。

### 合格テスト

- Xerosic の相手手札4--7枚の小型プール総当たりと、枚数ベクトル列挙の値・総質量が一致する。
- Xerosic で同じ手札multisetの全worldが同じ相手行動を使い、異なる手札観測では別行動を許す。
- 相手サイド配分だけが異なるworldを、Xerosic前に列挙しない。
- `pendingPlayer` を強制的に actor と異ならせ、相手領域だけが具体化される。
- Boss’s Orders と Enhanced Hammer で相手hidden world数が増えない。
- 固定デッキ同士のseed 6で、B由来の相手山札検索ノードが0である。
- 相手activeをきぜつさせる全経路で、相手の新active選択後にだけターン葉へ到達する。
- 相手priorなしでXerosicが必要なactionだけが `certified=false` となり、理由が専用集計される。
