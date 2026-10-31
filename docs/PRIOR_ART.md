# Prior art

The current implementation combines marginal admission gates, exponential time
decay, partition-relative concentration thresholds and a decayed weighted
SpaceSaving summary of encoded itemsets or numeric pairs. These are established
techniques.

A transaction with M items has $2^M - 1$ possible nonempty subsets. Candidate
generation determines which combinations reach the counters; heavy-hitter stores
then count the encoded itemset keys. Singleton gating omits earlier pattern history, while the configured maximum
itemset order limits the combinations considered.

## Ancestry

| Source | Inherited mechanism | Bishfpm adaptation and limits |
| --- | --- | --- |
| Metwally, Agrawal and El Abbadi, SpaceSaving [4] | Minimum-counter replacement with inherited admission error | Weighted string itemsets or numeric pairs, elapsed-time decay, and pruning that preserves omitted-history bounds. Intervals cover evaluated candidates; gates censor history and true top-k ranking is not guaranteed. |
| Homem and Carvalho, Filtered SpaceSaving [14]; the [`topk` 0.7.0 reference implementation](https://docs.rs/topk/0.7.0/src/topk/topk/mod.rs.html) | Bucket history filters misses before minimum-counter replacement | Numeric pairs, decayed bucket bounds, weighted/late updates, and history-preserving pruning. |

## Related work

The following works provide context for decay, transactional mining, relative
concentration and explanations. Conceptual parallels do not establish direct
implementation ancestry or transfer their guarantees.

- **Stream Frequency & Transactional Summaries**: Manku and Motwani [1] introduced
  Lossy Counting for approximate frequency over data streams, including an extension
  for transactional itemsets where buckets act as pruning/accounting intervals rather
  than generic bounded sliding windows. Giannella et al. (FP-Stream) [2] explored
  multiple time granularities for pattern history. Chi et al. (Moment) [3] tracked
  closed frequent itemsets over a sliding window.
- **Top-K and Recent Itemsets**: Wong and Fu [5] investigated bounded top-k
  frequent itemset tracking in data streams. Lim and Kang (TwMinSwap-Is) [6]
  proposed time-weighted recent itemset mining; their transaction-time model
  differs from bishfpm's elapsed-time clock.
- **Time Decay & Conditional Concentration**: Cormode et al. (Forward Decay) [7]
  formulated decayed streaming statistics and relative mass thresholds. Mirylenka et al. [8]
  developed conditional heavy hitters to detect items whose frequencies are high relative
  to a parent partition. Bailis et al. (MacroBase) [9] demonstrated streaming marginal
  filtering and combination explanation.
- **Sketched Counters & Bursts**: Yang et al. (HeavyGuardian) [10] introduced a
  summary that separates frequent and infrequent items, protecting frequent-item
  counters through probabilistic decay on collisions. Kleinberg [11] and
  Zhong / Miao et al. (BurstSketch) [12, 13] focused on inferring burst structures
  and change detection, which differs fundamentally from continuous concentrated
  support.

---

## Bibliography

[1] G. S. Manku and R. Motwani. Approximate Frequency Counts over Data Streams. In *Proceedings of the 28th International Conference on Very Large Data Bases (VLDB)*, pages 346–357, 2002. [https://www.vldb.org/conf/2002/S10P03.pdf](https://www.vldb.org/conf/2002/S10P03.pdf)

[2] C. Giannella, J. Han, J. Pei, X. Yan, and P. S. Yu. Mining Frequent Patterns in Data Streams at Multiple Time Granularities. In *Next Generation Data Mining*, pages 191–212, 2003. [https://hanj.cs.illinois.edu/pdf/fpstm03.pdf](https://hanj.cs.illinois.edu/pdf/fpstm03.pdf)

[3] Y. Chi, H. Wang, P. S. Yu, and R. R. Muntz. Moment: Maintaining Closed Frequent Itemsets over a Stream Sliding Window. In *Proceedings of the 4th IEEE International Conference on Data Mining (ICDM)*, pages 59–66, 2004. [https://doi.org/10.1109/ICDM.2004.10084](https://doi.org/10.1109/ICDM.2004.10084)

[4] A. Metwally, D. Agrawal, and A. El Abbadi. Efficient Computation of Frequent and Top-k Elements in Data Streams. In *Proceedings of the 10th International Conference on Database Theory (ICDT)*, pages 398–412, 2005. [https://doi.org/10.1007/978-3-540-30570-5_27](https://doi.org/10.1007/978-3-540-30570-5_27)

[5] R. C.-W. Wong and A. W.-C. Fu. Mining Top-k Frequent Itemsets from Data Streams. *Data Mining and Knowledge Discovery*, 13(2):193–217, 2006. [https://home.cse.hkust.edu.hk/~raywong/paper/dami06-topStream.pdf](https://home.cse.hkust.edu.hk/~raywong/paper/dami06-topStream.pdf)

[6] Y. Lim and U. Kang. Time-weighted counting for recently frequent pattern mining in data streams. *Knowledge and Information Systems*, 53(2):391–422, 2017. [https://doi.org/10.1007/s10115-017-1045-1](https://doi.org/10.1007/s10115-017-1045-1)

[7] G. Cormode, V. Shkapenyuk, D. Srivastava, and B. Xu. Forward Decay: A Practical Time Decay Model for Streaming Systems. In *IEEE 25th International Conference on Data Engineering (ICDE)*, pages 138–149, 2009. [https://doi.org/10.1109/ICDE.2009.65](https://doi.org/10.1109/ICDE.2009.65)

[8] K. Mirylenka, G. Cormode, T. Palpanas, and D. Srivastava. Conditional heavy hitters: detecting interesting correlations in data streams. *The VLDB Journal*, 24(3):395–414, 2015. [https://wrap.warwick.ac.uk/74443/8/WRAP_condhh-vldbj.pdf](https://wrap.warwick.ac.uk/74443/8/WRAP_condhh-vldbj.pdf)

[9] P. Bailis, E. Gan, S. Madden, D. Narayanan, K. Rong, and S. Suri. MacroBase: Prioritizing Attention in Fast Data. In *Proceedings of the 2017 ACM SIGMOD International Conference on Management of Data*, 2017. Also available as *arXiv:1603.00567*. [https://arxiv.org/abs/1603.00567](https://arxiv.org/abs/1603.00567)

[10] T. Yang, J. Gong, H. Zhang, L. Zou, L. Shi, and X. Li. HeavyGuardian: Separate and Guard Hot Items in Data Streams. In *Proceedings of the 24th ACM SIGKDD International Conference on Knowledge Discovery & Data Mining*, pages 2584–2593, 2018. [https://doi.org/10.1145/3219819.3219978](https://doi.org/10.1145/3219819.3219978)

[11] J. Kleinberg. Bursty and Hierarchical Structure in Streams. In *Proceedings of the 8th ACM SIGKDD International Conference on Knowledge Discovery and Data Mining*, pages 91–101, 2002. [https://doi.org/10.1145/775047.775061](https://doi.org/10.1145/775047.775061)

[12] Z. Zhong, S. Yan, Z. Li, D. Tan, T. Yang, and B. Cui. BurstSketch: Finding Bursts in Data Streams. In *Proceedings of the 2021 International Conference on Management of Data (SIGMOD)*, pages 2375–2383, 2021. [https://doi.org/10.1145/3448016.3452775](https://doi.org/10.1145/3448016.3452775)

[13] R. Miao, Z. Zhong, J. Guo, Z. Li, T. Yang, and B. Cui. BurstSketch: Finding Bursts in Data Streams. *IEEE Transactions on Knowledge and Data Engineering (TKDE)*, 35(11):11126–11140, 2023. [https://doi.org/10.1109/TKDE.2022.3223686](https://doi.org/10.1109/TKDE.2022.3223686)

[14] N. Homem and J. P. Carvalho. Finding top-k elements in data streams. *Information Sciences*, 180(24):4958–4974, 2010. [https://doi.org/10.1016/j.ins.2010.08.024](https://doi.org/10.1016/j.ins.2010.08.024)
