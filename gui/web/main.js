const devicesEl = document.getElementById("devices");
const healthEl = document.getElementById("health");
const healthSummaryEl = document.getElementById("health-summary");
const receiveHistoryEl = document.getElementById("receive-history");
const subtitleEl = document.getElementById("subtitle");

function mhz(value) {
  return (value / 1_000_000).toFixed(3);
}

function ageText(timestampMs) {
  const age = Math.max(0, Date.now() - timestampMs);
  return `${(age / 1000).toFixed(1)}s old`;
}

function lastScannedText(timestampMs) {
  if (!timestampMs) return "never";
  const age = Math.max(0, Date.now() - timestampMs);
  if (age < 1000) return "now";
  if (age < 60_000) return `${Math.round(age / 1000)}s ago`;
  return `${Math.floor(age / 60_000)}m ${Math.round((age % 60_000) / 1000)}s ago`;
}

function lastReceivedText(timestampMs) {
  if (!timestampMs) return "never";
  const age = Math.max(0, Date.now() - timestampMs);
  if (age < 1000) return "now";
  if (age < 60_000) return `${Math.round(age / 1000)}s ago`;
  if (age < 3_600_000) return `${Math.floor(age / 60_000)}m ago`;
  if (age < 86_400_000) return `${Math.floor(age / 3_600_000)}h ago`;
  return new Date(timestampMs).toLocaleString();
}

function clusterLastReceived(cluster) {
  return Math.max(0, ...(cluster.channels || []).map(channel => channel.last_received_ms || 0));
}

function channelSummary(channels) {
  return channels.map(channel => `
    <div class="channel-detail state-${channel.state}">
      <span class="channel-mhz">${mhz(channel.frequency)}</span>
      <span>${channel.state}</span>
      <span>hits ${channel.hits}</span>
      <span>rx ${lastReceivedText(channel.last_received_ms)}</span>
      <span class="channel-label">${channel.label || ""}</span>
    </div>
  `).join("");
}

function clusterState(cluster) {
  return (cluster.channels || []).some(channel => channel.state === "open") ? "open" : "idle";
}

function collectChannels(status) {
  const rows = [];
  for (const device of status.devices || []) {
    for (const cluster of device.clusters || []) {
      for (const channel of cluster.channels || []) {
        rows.push({
          ...channel,
          cluster_index: cluster.index,
          cluster_center: cluster.center_frequency,
          device_index: device.index,
          scanning: cluster.index === device.current_cluster,
        });
      }
    }
  }
  return rows;
}

function renderHealthSummary(status) {
  const devices = status.devices || [];
  const statusAgeMs = Math.max(0, Date.now() - status.timestamp_ms);
  const clusterDevices = devices.filter(device => device.mode === "cluster_scan");
  const inputOverflows = devices.reduce((sum, device) => sum + (device.input_overflows || 0), 0);
  const outputOverruns = devices.reduce((sum, device) => sum + (device.output_overruns || 0), 0);
  const activeChannels = collectChannels(status).filter(channel => channel.state === "open").length;
  const primaryDevice = clusterDevices[0] || devices[0] || {};
  const currentCluster = primaryDevice.cluster_count
    ? `${(primaryDevice.current_cluster || 0) + 1}/${primaryDevice.cluster_count}`
    : "n/a";
  const scanTimes = (primaryDevice.clusters || []).map(cluster => cluster.last_scan_ms).filter(Boolean);
  const sweepAge = scanTimes.length ? ageText(Math.min(...scanTimes)) : "never";

  healthSummaryEl.innerHTML = `
    <div class="summary-item"><span>Airband</span><strong>${statusAgeMs < 3000 ? "alive" : "stale"}</strong></div>
    <div class="summary-item"><span>Status age</span><strong>${ageText(status.timestamp_ms)}</strong></div>
    <div class="summary-item"><span>Mode</span><strong>${clusterDevices.length ? "cluster_scan" : "mixed"}</strong></div>
    <div class="summary-item"><span>Current cluster</span><strong>${currentCluster}</strong></div>
    <div class="summary-item"><span>Sweep age</span><strong>${sweepAge}</strong></div>
    <div class="summary-item"><span>Active channels</span><strong>${activeChannels}</strong></div>
    <div class="summary-item"><span>Input overflows</span><strong>${inputOverflows}</strong></div>
    <div class="summary-item"><span>Output overruns</span><strong>${outputOverruns}</strong></div>
  `;
}

function renderReceiveHistory(status) {
  const recent = collectChannels(status)
    .filter(channel => channel.last_received_ms)
    .sort((a, b) => b.last_received_ms - a.last_received_ms)
    .slice(0, 20);

  receiveHistoryEl.innerHTML = `
    <div class="section-title">
      <strong>Recently received</strong>
      <span>${recent.length ? `${recent.length} frequencies` : "No receive events yet"}</span>
    </div>
    <table class="history-table">
      <thead>
        <tr><th>Last received</th><th>MHz</th><th>State</th><th>Hits</th><th>Cluster</th><th>Label</th></tr>
      </thead>
      <tbody>
        ${recent.map(channel => `
          <tr class="${channel.scanning ? "active-cluster" : ""}">
            <td>${lastReceivedText(channel.last_received_ms)}</td>
            <td>${mhz(channel.frequency)}</td>
            <td class="state-${channel.state}">${channel.state}</td>
            <td>${channel.hits}</td>
            <td>${channel.cluster_index + 1} @ ${mhz(channel.cluster_center)}</td>
            <td class="history-label">${channel.label || ""}</td>
          </tr>
        `).join("") || `<tr><td colspan="6" class="empty-state">No received frequencies since last state reset.</td></tr>`}
      </tbody>
    </table>
  `;
}

function renderStatus(status) {
  healthEl.textContent = "online";
  healthEl.classList.add("online");
  subtitleEl.textContent = `status ${ageText(status.timestamp_ms)}`;
  renderHealthSummary(status);
  renderReceiveHistory(status);

  devicesEl.innerHTML = "";
  for (const device of status.devices || []) {
    const section = document.createElement("section");
    section.className = "device";

    const clusterNumber = (device.current_cluster ?? 0) + 1;
    const clusterCount = device.cluster_count ?? 0;
    const clusters = device.clusters || [];

    section.innerHTML = `
      <div class="device-header">
        <div><strong>Device ${device.index}</strong> ${device.mode}</div>
        <div class="metric">Cluster <strong>${clusterNumber}/${clusterCount}</strong></div>
        <div class="metric">Center <strong>${mhz(device.center_frequency || 0)} MHz</strong></div>
        <div class="metric">Span <strong>${mhz(device.min_frequency || 0)}-${mhz(device.max_frequency || 0)}</strong></div>
        <div class="metric">Input OF <strong>${device.input_overflows}</strong></div>
        <div class="metric">Output OR <strong>${device.output_overruns}</strong></div>
      </div>
      <table>
        <thead>
          <tr><th>Cluster</th><th>Center</th><th>Span</th><th>Freqs</th><th>State</th><th>Last scanned</th><th>Last received</th><th>Channels</th></tr>
        </thead>
        <tbody>
          ${clusters.map(cluster => {
            const state = clusterState(cluster);
            const active = cluster.index === device.current_cluster;
            const lastReceived = clusterLastReceived(cluster);
            return `
            <tr class="${active ? "active-cluster" : ""}">
              <td>${cluster.index + 1}</td>
              <td>${mhz(cluster.center_frequency)}</td>
              <td>${mhz(cluster.min_frequency)}-${mhz(cluster.max_frequency)}</td>
              <td>${(cluster.channels || []).length}</td>
              <td class="state-${active ? "scanning" : state}">${active ? "scanning" : state}</td>
              <td>${lastScannedText(cluster.last_scan_ms)}</td>
              <td>${lastReceivedText(lastReceived)}</td>
              <td class="channels">${channelSummary(cluster.channels || [])}</td>
            </tr>
          `;
          }).join("")}
        </tbody>
      </table>
    `;
    devicesEl.appendChild(section);
  }
}

async function refresh() {
  try {
    const response = await fetch("status.json", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderStatus(await response.json());
  } catch (error) {
    healthEl.textContent = "offline";
    healthEl.classList.remove("online");
    subtitleEl.textContent = error.message;
  }
}

setInterval(refresh, 1000);
refresh();
