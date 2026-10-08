import { useEffect, useRef } from "react";
const focusable =
  'button:not([disabled]), a[href], input:not([disabled]), select:not([disabled]), [tabindex="0"]';
export function useController(back: () => void, consoleBrowser = false) {
  const onBack = useRef(back);
  const onConsole = useRef(consoleBrowser);
  useEffect(() => {
    onBack.current = back;
    onConsole.current = consoleBrowser;
  }, [back, consoleBrowser]);
  useEffect(() => {
    let frame = 0,
      last = 0,
      held = "",
      enterHeld = false,
      dispatchingKey = false,
      clicking = false,
      cursorGuardUntil = 0,
      navigationOwned = false,
      cursorPress = false,
      cursorOnlyConfirm = false,
      nativeEchoUntil = 0;
    let pointerPosition: { x: number; y: number } | null = null;
    function sendKey(current: HTMLElement, key: string) {
      dispatchingKey = true;
      try {
        current.dispatchEvent(
          new KeyboardEvent("keydown", {
            key,
            bubbles: true,
            cancelable: true,
          }),
        );
      } finally {
        dispatchingKey = false;
      }
    }
    function browseSearchKey(key: string) {
      const current = document.activeElement as HTMLElement | null;
      if (
        current?.tagName !== "INPUT" ||
        current.getAttribute("aria-controls") !== "browse-results" ||
        current.closest("[inert]")
      )
        return false;
      const first = document
        .getElementById("browse-results")
        ?.querySelector<HTMLElement>(".game-tile");
      // Single-line Search has no vertical caret movement. Keep horizontal
      // arrows for editing, but provide an exit even when there are no results.
      const next =
        key === "ArrowUp"
          ? document.querySelector<HTMLElement>(
              '.main-nav [aria-current="page"]',
            )
          : key === "ArrowDown"
            ? first ||
              document.querySelector<HTMLElement>(
                '.browse .collection-tabs [aria-pressed="true"]',
              )
            : key === "Enter"
              ? first
              : null;
      if (!next) return false;
      next.focus();
      next.scrollIntoView({ block: "nearest", inline: "nearest" });
      return true;
    }
    function comboKey(key: string, onlyOpen = false) {
      const current = document.activeElement as HTMLElement | null;
      if (
        !current?.matches(
          onlyOpen
            ? '[role="combobox"][aria-expanded="true"]'
            : '[role="combobox"]',
        )
      )
        return false;
      sendKey(current, key);
      return true;
    }
    function goBack() {
      if (comboKey("Escape", true)) return;
      const close =
        document.querySelector<HTMLButtonElement>(
          '[role="dialog"] [data-dialog-back]',
        ) ||
        document.querySelector<HTMLButtonElement>('[role="dialog"] .close');
      if (close) close.click();
      else onBack.current();
    }
    function railKey(key: string) {
      const current = document.activeElement as HTMLElement | null;
      if (
        (key !== "ArrowLeft" && key !== "ArrowRight") ||
        !current?.closest("[data-virtual-rail]")
      )
        return false;
      // The row can mount its next tile before moving controller focus to it.
      sendKey(current, key);
      return true;
    }
    function scrollContent(direction: string) {
      if (direction !== "ArrowUp" && direction !== "ArrowDown") return false;
      const current = document.activeElement as HTMLElement | null;
      if (!current?.matches("[data-controller-scroll]")) return false;
      const region = current.closest<HTMLElement>("[data-scroll-region]");
      if (!region) return false;
      const down = direction === "ArrowDown";
      if (
        down
          ? region.scrollTop + region.clientHeight >= region.scrollHeight - 1
          : region.scrollTop <= 0
      )
        return false;
      region.scrollBy({ top: (down ? 1 : -1) * region.clientHeight * 0.7 });
      return true;
    }
    function move(direction: string) {
      if (scrollContent(direction)) return;
      if (browseSearchKey(direction)) return;
      const scope = document.querySelector('[role="dialog"]') || document;
      const items = [...scope.querySelectorAll<HTMLElement>(focusable)].filter(
        (el) => el.offsetWidth && el.offsetHeight && !el.closest("[inert]"),
      );
      const current = document.activeElement as HTMLElement,
        rect = current?.getBoundingClientRect();
      if (!rect || !items.includes(current)) {
        items[0]?.focus();
        return;
      }
      const x = rect.x + rect.width / 2,
        y = rect.y + rect.height / 2;
      const horizontal =
        direction === "ArrowLeft" || direction === "ArrowRight";
      const sign =
        direction === "ArrowLeft" || direction === "ArrowUp" ? -1 : 1;
      const next = items
        .filter((el) => el !== current)
        .map((el) => {
          const r = el.getBoundingClientRect(),
            dx = r.x + r.width / 2 - x,
            dy = r.y + r.height / 2 - y;
          const primary = (horizontal ? dx : dy) * sign,
            secondary = Math.abs(horizontal ? dy : dx);
          // Overlapping rows/columns are aligned even when their controls have
          // very different widths. Center distance alone can skip a wide card.
          const gap = horizontal
            ? Math.max(0, r.y - (rect.y + rect.height), rect.y - (r.y + r.height))
            : Math.max(0, r.x - (rect.x + rect.width), rect.x - (r.x + r.width));
          return { el, primary, gap, score: primary + gap * 3 + secondary * 0.01 };
        })
        .filter((i) => i.primary > 4)
        .sort(
          (a, b) => Number(a.gap > 0) - Number(b.gap > 0) || a.score - b.score,
        )[0];
      next?.el.focus();
      next?.el.scrollIntoView({
        block: "nearest",
        inline: "nearest",
        behavior: "smooth",
      });
    }
    // Rings follow the controller or keyboard; pointer and touch input hide them.
    const root = document.documentElement;
    function pointer() {
      root.classList.add("pointer");
      cursorGuardUntil = 0;
      navigationOwned = cursorPress = cursorOnlyConfirm = false;
      nativeEchoUntil = 0;
    }
    function pointerMove(e: MouseEvent) {
      // A stationary browser cursor must not take ownership back from D-pad
      // navigation. Real movement and touch still switch to pointer input.
      const moved =
        e.movementX ||
        e.movementY ||
        (pointerPosition &&
          (pointerPosition.x !== e.clientX || pointerPosition.y !== e.clientY));
      pointerPosition = { x: e.clientX, y: e.clientY };
      if (moved) pointer();
    }
    function clickFocused() {
      clicking = true;
      try {
        (document.activeElement as HTMLElement | null)?.click();
      } finally {
        clicking = false;
      }
    }
    function confirm() {
      root.classList.remove("pointer");
      // Some console browsers also emit a native cursor click for X. It may
      // arrive after key/button release, so keep a short compatibility window.
      cursorGuardUntil = performance.now() + 600;
      if (!comboKey("Enter")) clickFocused();
    }
    function cursorConfirm(e: MouseEvent | PointerEvent) {
      if (clicking || e.button !== 0) return;
      // Internal .click() calls (for example closing a modal) have their own
      // target. They are not the console browser's native cursor activation.
      if (e.isTrusted === false && e.detail === 0) return;
      if (
        ("pointerType" in e && e.pointerType !== "mouse") ||
        (
          e as MouseEvent & {
            sourceCapabilities?: { firesTouchEvents: boolean };
          }
        ).sourceCapabilities?.firesTouchEvents
      ) {
        pointer();
        return;
      }
      const select = !!navigator.getGamepads?.()[0]?.buttons[0]?.pressed;
      const ownsConfirm =
        !root.classList.contains("pointer") &&
        ((onConsole.current && navigationOwned) ||
          select ||
          enterHeld ||
          held === "select" ||
          performance.now() < cursorGuardUntil);
      if (!ownsConfirm) {
        if (e.type === "pointerdown" || e.type === "mousedown") pointer();
        return;
      }
      // Capture before the browser moves focus to the tile under its cursor.
      // Read the pad here too: native mouse events can precede our RAF poll.
      e.preventDefault();
      e.stopImmediatePropagation();
      const now = performance.now();
      const down = e.type === "pointerdown" || e.type === "mousedown";
      if (down) {
        // A new native-only press must work immediately, including a second
        // X used to confirm an open dropdown. Do not treat it as an echo.
        if (cursorOnlyConfirm && !select && !enterHeld && held !== "select") {
          cursorGuardUntil = nativeEchoUntil = 0;
          cursorOnlyConfirm = false;
        }
        cursorPress = true;
      }
      if (select && held !== "select" && !enterHeld) {
        held = "select";
        if (now >= nativeEchoUntil) {
          cursorOnlyConfirm = false;
          confirm();
        }
      } else if (e.type === "click") {
        if (
          !select &&
          !enterHeld &&
          held !== "select" &&
          (now >= cursorGuardUntil || (cursorOnlyConfirm && !cursorPress))
        ) {
          // Some PS5 browsers expose X only as a cursor click. Navigation
          // owns the target until the user actually moves the pointer/touches.
          confirm();
          cursorOnlyConfirm = true;
          nativeEchoUntil = now + 200;
        }
        cursorPress = false;
      }
    }
    // Confirm may arrive through keyboard events, gamepad polling, or both.
    // Capture it before a widget handles it so one press stays one action even
    // when opening a page moves focus to a different button.
    function confirmKey(e: KeyboardEvent) {
      if (
        !dispatchingKey &&
        !e.isComposing &&
        !e.altKey &&
        !e.ctrlKey &&
        !e.metaKey &&
        !e.shiftKey &&
        [
          "ArrowUp",
          "ArrowDown",
          "ArrowLeft",
          "ArrowRight",
          "Home",
          "End",
          "Tab",
        ].includes(e.key)
      ) {
        navigationOwned = true;
        root.classList.remove("pointer");
        cursorGuardUntil = nativeEchoUntil = 0;
        cursorOnlyConfirm = false;
      }
      if (
        e.key !== "Enter" ||
        e.isComposing ||
        dispatchingKey ||
        e.altKey ||
        e.ctrlKey ||
        e.metaKey ||
        e.shiftKey
      )
        return;
      const repeated =
        enterHeld ||
        e.repeat ||
        held === "select" ||
        performance.now() < nativeEchoUntil;
      enterHeld = true;
      navigationOwned = true;
      root.classList.remove("pointer");
      if (repeated) {
        e.preventDefault();
        e.stopPropagation();
        return;
      }
      const target = document.activeElement as HTMLElement | null;
      if (
        target?.matches(
          'button:not([disabled]):not([role="combobox"]), a[href]',
        )
      ) {
        e.preventDefault();
        e.stopPropagation();
        confirm();
      }
    }
    function releaseKey(e: KeyboardEvent) {
      if (e.key === "Enter") {
        if (enterHeld) cursorGuardUntil = performance.now() + 600;
        enterHeld = false;
      }
    }
    function blur() {
      enterHeld = false;
      cursorGuardUntil = 0;
      navigationOwned = cursorPress = cursorOnlyConfirm = false;
      nativeEchoUntil = 0;
    }
    function key(e: KeyboardEvent) {
      if (e.defaultPrevented || e.isComposing) return;
      root.classList.remove("pointer");
      if (e.key === "Escape" || e.key === "BrowserBack") {
        e.preventDefault();
        goBack();
        return;
      }
      if ((e.target as HTMLElement).matches("input, select, textarea")) {
        if (
          !e.shiftKey &&
          !e.altKey &&
          !e.ctrlKey &&
          !e.metaKey &&
          browseSearchKey(e.key)
        )
          e.preventDefault();
        return;
      }
      if (e.key.startsWith("Arrow")) {
        e.preventDefault();
        move(e.key);
      }
    }
    document.addEventListener("keydown", confirmKey, true);
    document.addEventListener("keyup", releaseKey, true);
    document.addEventListener("keydown", key);
    const cursorEvents = [
      "pointerdown",
      "mousedown",
      "mouseup",
      "click",
    ] as const;
    for (const name of cursorEvents)
      document.addEventListener(name, cursorConfirm, true);
    document.addEventListener("pointermove", pointerMove, { passive: true });
    document.addEventListener("mousemove", pointerMove, { passive: true });
    document.addEventListener("touchstart", pointer, { passive: true });
    window.addEventListener("blur", blur);
    function shortcut(command: string) {
      if (
        document.querySelector(
          '[role="dialog"], [role="combobox"][aria-expanded="true"]',
        )
      )
        return;
      const gameCommand =
        command === "search"
          ? "favorite"
          : command === "details"
            ? "details"
            : null;
      if (gameCommand) {
        const action = document.querySelector<HTMLButtonElement>(
          `.native-game [data-command="${gameCommand}"]:not([disabled])`,
        );
        if (action) {
          action.click();
          return;
        }
      }
      if (command === "details") return;
      if (command === "search") {
        document
          .querySelector<HTMLButtonElement>(
            '.utilities [data-command="search"]',
          )
          ?.click();
        return;
      }
      const tabs = [
        ...document.querySelectorAll<HTMLButtonElement>(".main-nav button"),
      ];
      const current = tabs.findIndex(
        (button) => button.getAttribute("aria-current") === "page",
      );
      const next = current + (command === "nextTab" ? 1 : -1);
      if (current >= 0 && next >= 0 && next < tabs.length) {
        tabs[next].click();
        tabs[next].focus({ preventScroll: true });
      }
    }
    function tick(now: number) {
      const pad = navigator.getGamepads?.()[0];
      if (pad) {
        const command = pad.buttons[0]?.pressed
          ? "select"
          : pad.buttons[1]?.pressed
            ? "back"
            : pad.buttons[2]?.pressed
              ? "details"
              : pad.buttons[3]?.pressed
                ? "search"
                : pad.buttons[4]?.pressed
                  ? "previousTab"
                  : pad.buttons[5]?.pressed
                    ? "nextTab"
                    : pad.buttons[12]?.pressed || pad.axes[1] < -0.6
                      ? "ArrowUp"
                      : pad.buttons[13]?.pressed || pad.axes[1] > 0.6
                        ? "ArrowDown"
                        : pad.buttons[14]?.pressed || pad.axes[0] < -0.6
                          ? "ArrowLeft"
                          : pad.buttons[15]?.pressed || pad.axes[0] > 0.6
                            ? "ArrowRight"
                            : "";
        if (
          command &&
          (command !== held ||
            (command.startsWith("Arrow") && now - last > 240))
        ) {
          root.classList.remove("pointer");
          navigationOwned = true;
          if (command === "select") {
            if (!enterHeld && now >= nativeEchoUntil) {
              cursorOnlyConfirm = false;
              confirm();
            }
          } else if (command === "back") goBack();
          else if (
            ["search", "details", "previousTab", "nextTab"].includes(command)
          )
            shortcut(command);
          else if (comboKey(command, true)) {
            // The dropdown owns its option navigation while it is open.
          } else if (railKey(command)) {
            // Virtual rows own horizontal navigation, including their edges.
          } else if (
            document.activeElement instanceof HTMLSelectElement &&
            (command === "ArrowUp" || command === "ArrowDown")
          ) {
            const select = document.activeElement;
            const direction = command === "ArrowUp" ? -1 : 1;
            let index = select.selectedIndex + direction;
            while (
              index >= 0 &&
              index < select.options.length &&
              select.options[index].disabled
            )
              index += direction;
            if (index >= 0 && index < select.options.length) {
              select.selectedIndex = index;
              select.dispatchEvent(new Event("change", { bubbles: true }));
            }
          } else move(command);
          last = now;
        }
        if (held === "select" && command !== "select") {
          cursorGuardUntil = now + 600;
          nativeEchoUntil = 0;
        }
        held = command;
      } else held = "";
      frame = requestAnimationFrame(tick);
    }
    frame = requestAnimationFrame(tick);
    return () => {
      document.removeEventListener("keydown", confirmKey, true);
      document.removeEventListener("keyup", releaseKey, true);
      document.removeEventListener("keydown", key);
      for (const name of cursorEvents)
        document.removeEventListener(name, cursorConfirm, true);
      document.removeEventListener("pointermove", pointerMove);
      document.removeEventListener("mousemove", pointerMove);
      document.removeEventListener("touchstart", pointer);
      window.removeEventListener("blur", blur);
      cancelAnimationFrame(frame);
    };
  }, []);
}
