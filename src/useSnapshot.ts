import { useCallback, useRef, useState } from "react";
import { shareSnapshot } from "./snapshot";

// Reconcile before calling React: even an updater that returns its old value
// can invoke its owning component once after an earlier state change.
export function useSnapshot<T>(initial: T) {
  const [value, setValue] = useState(initial);
  const current = useRef(value);
  const publish = useCallback((next: T) => {
    const shared = shareSnapshot(current.current, next);
    if (shared !== current.current) {
      current.current = shared;
      setValue(shared);
    }
  }, []);
  return [value, publish] as const;
}
