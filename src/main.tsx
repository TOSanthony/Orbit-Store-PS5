import { createRoot } from "react-dom/client";
import App from "./App";
import { loadLanguage } from "./i18n";
import "./styles.css";
// The catalogue loads first, so the first screen is already in the right language.
void loadLanguage().then(() =>
  createRoot(document.getElementById("root")!).render(<App />),
);
