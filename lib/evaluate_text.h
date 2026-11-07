/* private template: define EVALUATE_COMBINATIONS and UPDATE_TEXT */
/* inputs and all possible subset lengths were validated by prepare_event;
 * append each prefix once per order, preserving lexicographic enumeration */
static void EVALUATE_COMBINATIONS(bishfpm_eval_ctx_t *ctx, size_t count,
                                  size_t target, size_t start, size_t depth,
                                  const char **items, double *marginals,
                                  char *pattern, size_t length) {
  if (depth == target) {
    pattern[length] = 0;
    topk_result_t update = UPDATE_TEXT(ctx, pattern, length);
    bishfpm_pattern_pressure_t *p = &ctx->grp->pressure;
    p->candidates_evaluated++;
    if (ctx->weight == 0)
      p->zero_weight_queries++;
    else if (!update.accepted)
      p->rejected_updates++;
    else if (update.was_present)
      p->resident_updates++;
    else if (update.present)
      p->admissions++;
    else
      p->filtered_updates++;
    p->replacements += update.replaced;
    if (candidate_promoted(ctx, depth, update))
      notify_candidate(ctx, items, NULL, depth, update, pattern, length, 0,
                       update.hash, marginals);
    return;
  }
  size_t needed = target - depth;
  for (size_t i = start; i + needed <= count; i++) {
    size_t pos = length;
    if (depth)
      pattern[pos++] = BISHFPM_ITEM_DELIM;
    const prepared_item_t *item = &ctx->event->items[i];
    size_t item_length = item->length;
    items[depth] = item->key;
    marginals[depth] = item->marginal;
    memcpy(pattern + pos, item->key, item_length);
    EVALUATE_COMBINATIONS(ctx, count, target, i + 1, depth + 1, items,
                          marginals, pattern, pos + item_length);
  }
}

#undef EVALUATE_COMBINATIONS
#undef UPDATE_TEXT
