/* private template: event-local bindings and UPDATE_PAIR */
for (size_t i = 0; i < elevated; i++)
  for (size_t j = i + 1; j < elevated; j++) {
    uint64_t key = compact_key(event->ids[i], event->ids[j]);
    uint64_t hash = compact_hash(key);
    topk_result_t update = UPDATE_PAIR(key, hash);
    if (candidate_promoted(&ctx, 2, update)) {
      const char *items[] = {event->items[i].key, event->items[j].key};
      size_t lengths[] = {event->items[i].length, event->items[j].length};
      double marginals[] = {event->items[i].marginal, event->items[j].marginal};
      notify_candidate(&ctx, items, lengths, 2, update, NULL, 0, key, hash,
                       marginals);
    }
  }

#undef UPDATE_PAIR
