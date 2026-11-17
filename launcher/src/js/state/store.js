// A minimal observable store: the interface renders from its state and never keeps its own copy.

export function createStore(initial) {
  let state = initial;
  const subscribers = new Set();
  return {
    get: () => state,
    update(fn) {
      state = fn(state);
      for (const s of subscribers) s(state);
    },
    subscribe(fn) {
      subscribers.add(fn);
      return () => subscribers.delete(fn);
    },
  };
}
