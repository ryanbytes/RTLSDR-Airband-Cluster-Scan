const devicesEl = document.getElementById("devices");
const healthEl = document.getElementById("health");
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

function renderStatus(status) {
  healthEl.textContent = "online";
  healthEl.classList.add("online");
  subtitleEl.textContent = `status ${ageText(status.timestamp_ms)}`;

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
