import { memo, useEffect, useRef, useState } from "react";
import { Artwork } from "./Artwork";
import type { Game } from "./types";

// A working cover is not a substitute for a banner. Load them independently so
// DNS-blocked publisher art can never hold up the usable CDN colour backdrop.
type Props = {
  games: Game[];
  active: string;
  settle?: boolean;
  preferPublisherHero?: boolean;
};

export const Backdrop = memo(function Backdrop({
  games,
  active,
  settle = false,
  preferPublisherHero = false,
}: Props) {
  if (settle) return <SettledBackdrop games={games} active={active} />;
  const game = games.find((g) => g.id === active);
  return (
    <div className="backdrop" aria-hidden="true">
      {game && (
        <GameBackdrop
          key={`${game.id}\n${game.cover}\n${game.coverFallback}\n${game.hero}\n${game.heroFallback}\n${game.artworkLayout}\n${preferPublisherHero}`}
          game={game}
          preferPublisherHero={preferPublisherHero}
        />
      )}
    </div>
  );
});

function SettledBackdrop({ games, active }: Props) {
  const [shown, setShown] = useState(active);
  useEffect(() => {
    // Keep focus/title changes immediate, but don't fetch and decode a new
    // full-screen banner for every repeat while moving along a Discover row.
    const timer = window.setTimeout(() => setShown(active), 300);
    return () => window.clearTimeout(timer);
  }, [active]);
  return (
    <Backdrop
      games={games}
      active={games.some((game) => game.id === shown) ? shown : active}
    />
  );
}

function GameBackdrop({
  game,
  preferPublisherHero = false,
}: {
  game: Game;
  preferPublisherHero?: boolean;
}) {
  const [{ wide, attempt }, setImage] = useState({ wide: false, attempt: 0 });
  const bannerImage = useRef<HTMLImageElement>(null);
  // Some publisher pages only provided another square cover. Don't fetch that
  // twice, and don't stretch it into a banner if its dimensions aren't wide.
  const catalogBanners = [
    ...(game.artworkLayout === "wide" ? [game.hero] : []),
    game.heroFallback,
  ];
  // Details use the publisher's full scene when available: some console pic0
  // images are only a splash logo. Keep the CDN banner as a working fallback.
  const banners = (
    preferPublisherHero
      ? [game.heroFallback, ...catalogBanners]
      : catalogBanners
  ).filter(
    (url, index, urls) =>
      !!url &&
      url !== game.cover &&
      url !== game.coverFallback &&
      urls.indexOf(url) === index,
  );
  const banner = banners[attempt];
  const retry = () => {
    // A late event from the previous image must not skip the next candidate.
    setImage((current) =>
      current.attempt === attempt
        ? { attempt: current.attempt + 1, wide: false }
        : current,
    );
  };
  function imageReady(image: HTMLImageElement) {
    if (
      image.naturalHeight > 0 &&
      image.naturalWidth >= image.naturalHeight * 1.4
    )
      setImage((current) =>
        current.attempt === attempt && !current.wide
          ? { ...current, wide: true }
          : current,
      );
    else retry();
  }
  useEffect(() => {
    const image = bannerImage.current;
    if (!image || !banner) return;
    // Discover -> details remounts this element using an already-cached URL.
    // Do not depend solely on a new load event to reveal decoded artwork.
    const revealCached = () => {
      if (image.complete && image.naturalWidth > 0) imageReady(image);
    };
    revealCached();
    const frame = window.requestAnimationFrame(revealCached);
    return () => window.cancelAnimationFrame(frame);
  }, [banner, attempt]);
  return (
    <>
      {!wide && (
        <div className="backdrop-layer ambient active">
          <Artwork
            src={game.cover}
            fallbackSrc={game.coverFallback}
            alt=""
            priority
            decoding="async"
            referrerPolicy="no-referrer"
          />
        </div>
      )}
      {!!banner && (
        <div className={`backdrop-layer wide${wide ? " active" : ""}`}>
          <img
            key={banner}
            ref={bannerImage}
            src={banner}
            alt=""
            fetchPriority="high"
            decoding="async"
            referrerPolicy="no-referrer"
            onLoad={(event) => imageReady(event.currentTarget)}
            onError={retry}
          />
        </div>
      )}
    </>
  );
}
