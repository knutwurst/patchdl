const API = {
  status: "/api/status",
  titles: "/api/titles",
  config: "/api/config",
  downloads: "/api/downloads",
  action: (titleId, action) => `/api/titles/${encodeURIComponent(titleId)}/${action}`,
};

const SOURCE_TYPES = {
  official: {
    label: "Official",
    className: "ok",
    installAllowed: true,
    description: "Native console installation",
  },
  external: {
    label: "External",
    className: "external",
    installAllowed: true,
    description: "Real external installation",
  },
  shadowmount: {
    label: "Shadowmount",
    className: "shadow",
    installAllowed: false,
    description: "Download only",
  },
  unknown: {
    label: "Unknown",
    className: "blocked",
    installAllowed: false,
    description: "Blocked",
  },
};

const fallback = {
  status: {
    firmware: "11.60",
    firmware_build: "0x11600000",
    dns_guard: "Active",
    resolver: "Internal allowlist",
    free_space_mb: 64218,
    download_dir: "/data/patchdl (internal)",
  },
  config: {
    default_policy: "deny",
    download_dir: "/data/patchdl (internal)",
    install_after_download: false,
    delete_pkg_after_install: true,
    source_policy: {
      official: { allow_check: true, allow_download: true, allow_install: true },
      external: { allow_check: true, allow_download: true, allow_install: true },
      shadowmount: { allow_check: true, allow_download: true, allow_install: false },
      unknown: { allow_check: true, allow_download: false, allow_install: false },
    },
    cdn_allowlist: [
      "sgst.prod.dl.playstation.net",
      "gst.prod.dl.playstation.net",
      "gs2.ww.prod.dl.playstation.net",
    ],
  },
  titles: [
    {
      title_id: "PPSA01628_00",
      name: "Call of Duty Black Ops Cold War",
      content_id: "UP0002-PPSA01628_00-CODCWTHEGAME0001",
      installed_version: "01.032.000",
      compatible_version: "01.041.000",
      latest_version: "01.041.000",
      latest_required_fw: "11.60",
      source_type: "official",
      source_path: "/system_ex/app/PPSA01628_00",
      mount_from: "/dev/ssd0.system_ex",
      enabled: true,
      mode: "latest_compatible",
      queued: false,
      status: "available",
    },
    {
      title_id: "PPSA01284_00",
      name: "Demon's Souls",
      content_id: "UP9000-PPSA01284_00-DEMONSSOULS00001",
      installed_version: "01.002.000",
      compatible_version: "01.004.000",
      latest_version: "01.004.000",
      latest_required_fw: "10.01",
      source_type: "external",
      source_path: "/system_data/priv/appmeta/external/PPSA01284_00",
      mount_from: "/mnt/ext0/user/app/PPSA01284_00",
      enabled: true,
      mode: "pin",
      max_content_ver: "01.004.000",
      queued: true,
      status: "queued",
    },
    {
      title_id: "PPSA08329_00",
      name: "Example Future Game",
      content_id: "EP0000-PPSA08329_00-FUTUREPATCH00001",
      installed_version: "01.000.000",
      compatible_version: null,
      latest_version: "01.012.000",
      latest_required_fw: "12.50",
      source_type: "unknown",
      source_path: "/system_ex/app/PPSA08329_00",
      mount_from: "",
      enabled: false,
      mode: "disabled",
      queued: false,
      status: "blocked",
    },
    {
      title_id: "PPSA90001_00",
      name: "Shadowmounted Test Title",
      content_id: "UP0000-PPSA90001_00-SHADOWMOUNT0001",
      installed_version: "01.000.000",
      compatible_version: "01.006.000",
      latest_version: "01.009.000",
      latest_required_fw: "12.00",
      source_type: "shadowmount",
      source_path: "/system_ex/app/PPSA90001_00",
      mount_from: "/mnt/usb0/itemzflow/Shadowmounted Test Title",
      enabled: true,
      mode: "download_only",
      queued: false,
      status: "available",
    },
  ],
  downloads: [],
  logs: [],
};

let state = {
  status: fallback.status,
  config: fallback.config,
  titles: fallback.titles,
  downloads: fallback.downloads,
  logs: fallback.logs,
  filter: "all",
  query: "",
  usingFallback: false,
};
let downloadPollTimer = null;

const els = {};

document.addEventListener("DOMContentLoaded", () => {
  bindElements();
  bindEvents();
  loadInitialData();
});

function bindElements() {
  Object.assign(els, {
    firmwareValue: document.getElementById("firmwareValue"),
    firmwareBuild: document.getElementById("firmwareBuild"),
    dnsValue: document.getElementById("dnsValue"),
    resolverValue: document.getElementById("resolverValue"),
    spaceValue: document.getElementById("spaceValue"),
    downloadDirValue: document.getElementById("downloadDirValue"),
    gameGrid: document.getElementById("gameGrid"),
    queueList: document.getElementById("queueList"),
    logOutput: document.getElementById("logOutput"),
    allowlistHosts: document.getElementById("allowlistHosts"),
    sourcePolicyList: document.getElementById("sourcePolicyList"),
    searchInput: document.getElementById("searchInput"),
    defaultPolicy: document.getElementById("defaultPolicy"),
    downloadDir: document.getElementById("downloadDir"),
    installAfterDownload: document.getElementById("installAfterDownload"),
    deleteAfterInstall: document.getElementById("deleteAfterInstall"),
    refreshBtn: document.getElementById("refreshBtn"),
    saveBtn: document.getElementById("saveBtn"),
    pauseAllBtn: document.getElementById("pauseAllBtn"),
    toast: document.getElementById("toast"),
  });
}

function bindEvents() {
  els.searchInput.addEventListener("input", (event) => {
    state.query = event.target.value.trim().toLowerCase();
    renderGames();
  });

  document.querySelectorAll(".segmented button").forEach((button) => {
    button.addEventListener("click", () => {
      document.querySelectorAll(".segmented button").forEach((item) => item.classList.remove("is-selected"));
      button.classList.add("is-selected");
      state.filter = button.dataset.filter;
      renderGames();
    });
  });

  els.refreshBtn.addEventListener("click", loadInitialData);
  els.saveBtn.addEventListener("click", saveConfig);
  els.pauseAllBtn.addEventListener("click", () => showToast("Queue paused."));
}

async function loadInitialData() {
  state.usingFallback = false;
  const [status, config, titles, downloads] = await Promise.all([
    getJson(API.status, fallback.status),
    getJson(API.config, fallback.config),
    getJson(API.titles, fallback.titles),
    getJson(API.downloads, fallback.downloads),
  ]);

  // /api/titles has no client-only progress fields, so carry them across a
  // refresh — otherwise an in-flight download/install would flip the card back
  // to a clickable "Download/Install" mid-operation.
  const prev = new Map(state.titles.map((g) => [g.title_id, g]));
  titles.forEach((g) => {
    const old = prev.get(g.title_id);
    if (!old) return;
    if (old.downloading) g.downloading = true;
    if (old.downloaded) g.downloaded = true;
    if (old.installing) g.installing = true;
  });

  state = {
    ...state,
    status,
    config,
    titles,
    downloads,
  };

  render();
  if (state.downloads.length) startDownloadPolling();
  showToast(state.usingFallback ? "Demo data loaded. API is not reachable yet." : "Data refreshed.");
}

async function getJson(url, fallbackValue) {
  try {
    const response = await fetch(url, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return await response.json();
  } catch (error) {
    state.usingFallback = true;
    return clone(fallbackValue);
  }
}

function render() {
  renderStatus();
  renderSettings();
  renderGames();
  renderQueue();
  renderLogs();
}

function renderStatus() {
  els.firmwareValue.textContent = state.status.firmware || "--";
  els.firmwareBuild.textContent = state.status.firmware_build || "System version";
  els.dnsValue.textContent = state.status.dns_guard || "--";
  els.resolverValue.textContent = state.status.resolver || "--";
  els.spaceValue.textContent = formatMb(state.status.free_space_mb);
  els.downloadDirValue.textContent = state.status.download_dir || state.config.download_dir || "Download target";
}

function renderSettings() {
  els.defaultPolicy.value = state.config.default_policy || "deny";
  els.downloadDir.value = state.config.download_dir || "";
  els.installAfterDownload.checked = Boolean(state.config.install_after_download);
  els.deleteAfterInstall.checked = Boolean(state.config.delete_pkg_after_install);
  els.allowlistHosts.replaceChildren(...(state.config.cdn_allowlist || []).map((host) => {
    const chip = document.createElement("span");
    chip.className = "host-chip";
    chip.textContent = host;
    return chip;
  }));
  renderSourcePolicies();
}

function renderGames() {
  const visible = state.titles.filter(matchesFilter).filter(matchesQuery);
  els.gameGrid.replaceChildren();

  if (!visible.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No titles match this filter.";
    els.gameGrid.appendChild(empty);
    return;
  }

  visible.forEach((game) => els.gameGrid.appendChild(createGameCard(game)));
}

// One mutually-exclusive "what can I do with this title" bucket per game.
//   updatable : a newer compatible patch exists AND this source may be installed
//   uptodate  : already on the newest compatible patch
//   needsfw   : a newer patch exists but needs a newer firmware than installed
//   blocked   : source can't be safely patched (shadowmount / preinstall / unknown)
//   checking  : version lookup still running
function gameCategory(game) {
  if (game.installing) return "updatable"; // keep visible while the patch installs
  if (game.status === "checking") return "checking";
  if (game.patch_title_match === false) return "blocked"; // target-title mismatch
  if (!sourcePolicy(game).allow_install) return "blocked";
  if (game.status === "available") return "updatable";
  if (game.status === "incompatible_fw") return "needsfw";
  return "uptodate";
}

function matchesFilter(game) {
  if (state.filter === "all") return true;
  if (state.filter === "queued") return Boolean(game.queued) || game.status === "queued";
  return gameCategory(game) === state.filter;
}

function matchesQuery(game) {
  if (!state.query) return true;
  return [game.name, game.title_id, game.content_id]
    .concat([game.source_type, game.source_path, game.mount_from])
    .filter(Boolean)
    .some((value) => value.toLowerCase().includes(state.query));
}

function createGameCard(game) {
  const card = document.createElement("article");
  card.className = "game-card";

  const title = document.createElement("div");
  title.className = "game-title";
  const installBlocked = isInstallBlocked(game);
  title.innerHTML = `
    <div class="lead">
      <div class="cover">${initials(game.name)}</div>
    </div>
    <div>
      <h3>${escapeHtml(game.name)}</h3>
      <div class="subline">
        <span>${escapeHtml(game.title_id)}</span>
        <span>${escapeHtml(game.content_id || "")}</span>
      </div>
      <div class="subline" style="margin-top:8px">
        ${statusPill(game)}
        ${sourcePill(game)}
        ${installBlocked ? `<span class="pill blocked">Install blocked</span>` : `<span class="pill ok">Install allowed</span>`}
      </div>
      <div class="source-path">${escapeHtml(sourceDetail(game))}</div>
    </div>
  `;

  // Per-game enable/disable toggle, grouped under the game icon on the left
  // (only for sources that can actually be patched).
  if (sourcePolicy(game).allow_install) {
    const lead = title.querySelector(".lead");
    const toggle = document.createElement("label");
    toggle.className = "toggle";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = game.enabled !== false;
    cb.setAttribute("aria-label", `Enable updates for ${game.name}`);
    const track = document.createElement("span");
    track.className = "track";
    const lbl = document.createElement("span");
    lbl.className = "label";
    lbl.textContent = cb.checked ? "On" : "Off";
    cb.addEventListener("change", () => {
      lbl.textContent = cb.checked ? "On" : "Off";
      updateGame(game.title_id, { enabled: cb.checked, mode: cb.checked ? "latest_compatible" : "disabled" });
    });
    toggle.append(cb, track, lbl);
    lead.appendChild(toggle);
  }

  const versions = document.createElement("div");
  versions.className = "version-grid";
  versions.innerHTML = `
    ${versionBox("Installed", game.installed_version || "--")}
    ${versionBox("Compatible", game.compatible_version || "None")}
    ${versionBox("Latest", `${game.latest_version || "--"} / FW ${game.latest_required_fw || "--"}`)}
  `;

  const controls = document.createElement("div");
  controls.className = "controls";

  const actions = document.createElement("div");
  actions.className = "actions-row";
  const act = rowAction(game);
  if (act) {
    const btn = document.createElement("button");
    btn.className = "row-button is-primary";
    btn.innerHTML = `<svg><use href="#icon-download"></use></svg> ${escapeHtml(act.label)}`;
    btn.title = escapeHtml(act.hint || act.label);
    if (act.disabled || !act.action) {
      btn.disabled = true;
    } else {
      btn.addEventListener("click", () => runTitleAction(game.title_id, act.action));
    }
    actions.appendChild(btn);
  }

  // Cancel an in-progress download, or delete a finished/leftover one.
  if (game.downloading || game.downloaded) {
    const del = document.createElement("button");
    del.className = "row-button is-danger";
    del.textContent = game.downloading ? "Cancel" : "Delete";
    del.title = game.downloading
      ? "Stop the download and delete the partial file"
      : "Delete the downloaded package";
    del.addEventListener("click", () => cancelDownload(game.title_id));
    actions.appendChild(del);
  }

  controls.append(actions);
  card.append(title, versions, controls);
  return card;
}

function versionBox(label, value) {
  return `
    <div class="version-box">
      <span>${escapeHtml(label)}</span>
      <strong>${escapeHtml(value)}</strong>
    </div>
  `;
}

// Status pill = update state only (version/firmware/target title). The source type
// has its own pill, so this one never repeats "Shadowmount" etc.
function statusPill(game) {
  if (game.installing) return `<span class="pill warn">Installing…</span>`;
  if (game.status === "checking") return `<span class="pill">Checking…</span>`;
  if (game.queued || game.status === "queued") return `<span class="pill warn">In queue</span>`;
  if (game.patch_title_match === false) return `<span class="pill blocked">Title mismatch</span>`;
  if (game.status === "available") return `<span class="pill ok">Update available</span>`;
  if (game.status === "incompatible_fw") return `<span class="pill warn">Needs FW ${escapeHtml(game.latest_required_fw || "")}</span>`;
  if (game.status === "up_to_date") return `<span class="pill">Up to date</span>`;
  return `<span class="pill">No patch info</span>`;
}

function sourcePill(game) {
  const info = sourceInfo(game);
  return `<span class="pill ${info.className}">${escapeHtml(info.label)}</span>`;
}

function sourceInfo(game) {
  return SOURCE_TYPES[sourceType(game)] || SOURCE_TYPES.unknown;
}

function sourceType(game) {
  return game.source_type || "unknown";
}

function sourceDetail(game) {
  const src = sourceType(game);
  if (src === "shadowmount") return `Mount: ${game.mount_from || "unknown source"}`;
  if (src === "external") return `External: ${game.source_path || "external app metadata"}`;
  if (src === "official") return game.source_path || "Official installation";
  return "Source could not be identified";
}

function renderQueue() {
  els.queueList.replaceChildren();
  if (!state.downloads.length) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No active downloads.";
    els.queueList.appendChild(empty);
    return;
  }

  state.downloads.forEach((item) => {
    const pct = Math.max(0, Math.min(100, Number(item.progress) || 0));
    const row = document.createElement("div");
    row.className = "queue-item";

    const info = document.createElement("div");
    info.innerHTML = `
      <strong>${escapeHtml(item.name)}</strong>
      <span>${escapeHtml(item.title_id)} - ${escapeHtml(item.version)} - ${escapeHtml(item.detail || "")}</span>
      <div class="progress"><i style="width:${pct}%"></i></div>
    `;

    const right = document.createElement("div");
    right.className = "queue-actions";
    const span = document.createElement("span");
    span.textContent = `${pct}%`;
    const cancel = document.createElement("button");
    cancel.className = "row-button is-danger";
    cancel.textContent = "Cancel";
    cancel.title = "Stop the download and delete the partial file";
    cancel.addEventListener("click", () => cancelDownload(item.title_id));
    right.append(span, cancel);

    row.append(info, right);
    els.queueList.appendChild(row);
  });
}

function renderSourcePolicies() {
  const rows = Object.keys(SOURCE_TYPES).map((type) => {
    const info = SOURCE_TYPES[type];
    const policy = sourcePolicyForType(type);
    const row = document.createElement("div");
    row.className = "policy-row";
    row.innerHTML = `
      <span class="pill ${info.className}">${escapeHtml(info.label)}</span>
      <strong>${policy.allow_install ? "Install allowed" : "Install blocked"}</strong>
      <em>${policy.allow_download ? "Download allowed" : "Download blocked"} - ${escapeHtml(info.description)}</em>
    `;
    return row;
  });
  els.sourcePolicyList.replaceChildren(...rows);
}

function renderLogs() {
  els.logOutput.textContent = state.logs.length ? state.logs.join("\n") : "No events yet.";
}

let emptyPolls = 0;

function startDownloadPolling() {
  emptyPolls = 0;
  if (downloadPollTimer) return;
  downloadPollTimer = setInterval(refreshDownloads, 2000);
  refreshDownloads();
}

function stopDownloadPolling() {
  if (!downloadPollTimer) return;
  clearInterval(downloadPollTimer);
  downloadPollTimer = null;
}

async function refreshDownloads() {
  try {
    const response = await fetch(API.downloads, { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const downloads = await response.json();
    state.downloads = downloads;
    renderQueue();
    // A single empty poll can happen between manifest fetch and first piece;
    // only give up after a few in a row so a slow multi-GB job isn't dropped.
    if (!downloads.length && !state.titles.some((game) => game.downloading)) {
      if (++emptyPolls >= 3) stopDownloadPolling();
    } else {
      emptyPolls = 0;
    }
  } catch (error) {
    /* Keep the last local queue row visible if polling fails transiently. */
  }
}

// Cancel a running download (server aborts and deletes the partial) or delete
// an already-downloaded package. Same endpoint handles both.
async function cancelDownload(titleId) {
  const game = state.titles.find((g) => g.title_id === titleId);
  try {
    await postJson(API.action(titleId, "cancel"), {});
    showToast(`${game ? game.name : titleId}: download cancelled / deleted.`);
  } catch (error) {
    showToast(`${game ? game.name : titleId}: ${reasonText(error)}`);
  }
  if (game) {
    game.downloading = false;
    game.downloaded = false;
  }
  state.downloads = state.downloads.filter((item) => item.title_id !== titleId);
  state.logs.push(`[${timeNow()}] Download cancelled / deleted: ${titleId}`);
  renderGames();
  renderQueue();
  renderLogs();
  refreshDownloads();
}

async function saveConfig() {
  const config = {
    ...state.config,
    default_policy: els.defaultPolicy.value,
    download_dir: els.downloadDir.value.trim(),
    install_after_download: els.installAfterDownload.checked,
    delete_pkg_after_install: els.deleteAfterInstall.checked,
  };

  try {
    await postJson(API.config, config);
    state.config = config;
    showToast("Configuration saved.");
  } catch (error) {
    state.config = config;
    showToast("Configuration updated locally. API is not reachable yet.");
  }

  renderSettings();
}

// One per-row action button. Returns null when there is nothing to do.
//   install_after_download ON  -> "Update" (download + install in one go)
//   install_after_download OFF -> "Download", then "Install" once downloaded
//   download-only source       -> "Download" (no install step)
function rowAction(game) {
  if (game.installing) return { label: "Installing…", disabled: true };
  if (game.downloading) return { label: "Downloading…", disabled: true };
  if (game.patch_title_match === false) return null;     // target-title mismatch
  if (game.status !== "available") return null;          // nothing newer & compatible
  if (!isInstallAllowed(game)) return null;              // not a patchable source (e.g. shadowmount)
  if (state.config.install_after_download)
    return { label: "Update", action: "update", hint: "Download and install the update." };
  return game.downloaded
    ? { label: "Install", action: "install", hint: "Install the downloaded patch (modifies the game)." }
    : { label: "Download", action: "download", hint: "Download the patch internally." };
}

async function doDownload(game) {
  game.downloading = true;
  state.downloads = state.downloads.filter((item) => item.title_id !== game.title_id);
  state.downloads.push({
    title_id: game.title_id,
    name: game.name,
    version: game.compatible_version || "",
    progress: 0,
    detail: "Downloading manifest package internally. Large PS5 patches can take a long time.",
  });
  state.logs.push(`[${timeNow()}] Download started: ${game.title_id} ${game.compatible_version}`);
  renderQueue();
  renderLogs();
  renderGames();
  startDownloadPolling();
  let r;
  try {
    r = await postJson(API.action(game.title_id, "download"), {});
  } catch (error) {
    game.downloading = false;
    state.downloads = state.downloads.filter((item) => item.title_id !== game.title_id);
    const why = reasonText(error);
    state.logs.push(`[${timeNow()}] download ${game.title_id} blocked: ${why}`);
    showToast(`${game.name}: ${why}`);
    renderGames();
    renderQueue();
    renderLogs();
    stopDownloadPolling();
    return false;
  }
  game.downloading = false;
  state.downloads = state.downloads.filter((item) => item.title_id !== game.title_id);

  // A cancel (or soft failure) comes back as HTTP 200 with ok:false, so it
  // isn't thrown — handle it here instead of marking the patch downloaded.
  if (!r || r.ok === false) {
    game.downloaded = false;
    const what = r && r.cancelled ? "cancelled" : "failed";
    state.logs.push(`[${timeNow()}] Download ${what}: ${game.title_id}`);
    showToast(`${game.name}: download ${what}.`);
    renderGames();
    renderQueue();
    renderLogs();
    stopDownloadPolling();
    return false;
  }

  game.downloaded = true;
  const mb = r && r.bytes ? (r.bytes / (1024 * 1024)).toFixed(1) : "?";
  state.logs.push(`[${timeNow()}] Downloaded ${game.title_id} ${game.compatible_version} (${mb} MB, internal)`);
  showToast(`${game.name}: downloaded ${mb} MB.`);
  renderGames();
  renderQueue();
  renderLogs();
  stopDownloadPolling();
  return true;
}

async function doInstall(game) {
  game.installing = true;
  renderGames();
  try {
    await postJson(API.action(game.title_id, "install"), {});
  } catch (error) {
    game.installing = false;
    const why = reasonText(error);
    state.logs.push(`[${timeNow()}] install ${game.title_id} blocked: ${why}`);
    showToast(`${game.name}: ${why}`);
    renderGames();
    renderLogs();
    return false;
  }
  // rc=0 = accepted; keep "Installing…" (the PS5 applies it in the background;
  // it clears on the next refresh once the version updates).
  state.logs.push(`[${timeNow()}] Install started for ${game.title_id} ${game.compatible_version} — running in PS5 background`);
  showToast(`${game.name}: installing update — progress shows in your PS5 notifications.`);
  renderGames();
  renderLogs();
  return true;
}

async function runTitleAction(titleId, action) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;

  if (action === "download") await doDownload(game);
  else if (action === "install") await doInstall(game);
  else if (action === "update") { if (await doDownload(game)) await doInstall(game); }
}

function updateGame(titleId, patch) {
  const game = state.titles.find((item) => item.title_id === titleId);
  if (!game) return;
  if (sourceType(game) === "unknown" && patch.mode && !["disabled", "check_only"].includes(patch.mode)) {
    patch.mode = "check_only";
    patch.enabled = true;
    showToast(`${titleId}: unknown source, check only is allowed.`);
  }
  Object.assign(game, patch);
  if (patch.mode === "disabled") game.enabled = false;
  renderGames();

  // Persist the toggle on the PS5 (survives refresh and payload restart).
  const wantEnabled = game.enabled !== false;
  postJson(API.action(titleId, wantEnabled ? "enable" : "disable"), {})
    .then(() => {
      state.logs.push(`[${timeNow()}] Saved: ${titleId} ${wantEnabled ? "enabled" : "disabled"}`);
      renderLogs();
    })
    .catch(() => {
      showToast(`${titleId}: could not save toggle (API not reachable).`);
    });
}

function isDownloadAllowed(game) {
  const policy = sourcePolicy(game);
  return Boolean(game.enabled && game.compatible_version && policy.allow_download);
}

function isInstallBlocked(game) {
  return !sourcePolicy(game).allow_install;
}
function isInstallAllowed(game) {
  return Boolean(
    game.enabled &&
    game.compatible_version &&
    game.patch_title_match !== false &&
    sourcePolicy(game).allow_install
  );
}
function installTitle(game) {
  if (game.patch_title_match === false) return "Patch metadata targets a different title - install blocked.";
  if (isInstallBlocked(game)) return "Install blocked for this source.";
  if (!game.compatible_version) return "No compatible update to install.";
  return "Install the downloaded patch (modifies the game).";
}

function sourcePolicy(game) {
  return sourcePolicyForType(sourceType(game));
}

function sourcePolicyForType(type) {
  const fallbackPolicy = fallback.config.source_policy[type] || fallback.config.source_policy.unknown;
  const configuredPolicy = (state.config.source_policy || {})[type] || {};
  return { ...fallbackPolicy, ...configuredPolicy };
}

function downloadTitle(game) {
  if (!game.enabled) return "Title is disabled.";
  if (!game.compatible_version) return "No compatible update version is available.";
  if (!sourcePolicy(game).allow_download) return "Downloads are blocked for this source.";
  if (isInstallBlocked(game)) return "Download allowed; installs stay blocked for this source.";
  return "Download compatible update.";
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const err = new Error(data.reason || data.message || `HTTP ${response.status}`);
    err.body = data;
    err.status = response.status;
    throw err;
  }
  return data;
}

const REASON_TEXT = {
  patch_title_mismatch: "Patch metadata targets a different title - install blocked.",
  install_not_allowed_for_source: "Install blocked for this source.",
  source_unknown: "Source unknown — blocked.",
  no_compatible_patch: "No compatible patch available.",
  download_failed: "Download failed.",
  download_in_progress: "Another download is already running.",
};
function reasonText(error) {
  const r = error && error.body && error.body.reason;
  return (r && REASON_TEXT[r]) || (error && error.message) || "failed";
}

function initials(name) {
  return (name || "PD")
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0])
    .join("")
    .toUpperCase();
}

function formatMb(value) {
  if (!Number.isFinite(Number(value))) return "--";
  if (value > 1024) return `${(value / 1024).toFixed(1)} GB`;
  return `${value} MB`;
}

function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function timeNow() {
  return new Date().toLocaleTimeString("en-US", { hour12: false });
}

let toastTimer;
function showToast(message) {
  els.toast.textContent = message;
  els.toast.classList.add("is-visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => els.toast.classList.remove("is-visible"), 2600);
}
