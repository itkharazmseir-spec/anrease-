const TOKEN_KEY = "ao_store_token";

const loginSection = document.getElementById("login-section");
const catalogSection = document.getElementById("catalog-section");
const accountBar = document.getElementById("account-bar");
const accountUsernameEl = document.getElementById("account-username");
const accountIdEl = document.getElementById("account-id");
const loginForm = document.getElementById("login-form");
const loginError = document.getElementById("login-error");
const catalogGrid = document.getElementById("catalog-grid");
const purchaseMessage = document.getElementById("purchase-message");

function getToken() {
  return localStorage.getItem(TOKEN_KEY);
}
function setToken(token) {
  if (token) localStorage.setItem(TOKEN_KEY, token);
  else localStorage.removeItem(TOKEN_KEY);
}

async function api(path, { method = "GET", body } = {}) {
  const headers = {};
  const token = getToken();
  if (token) headers["Authorization"] = "Bearer " + token;
  if (body) headers["Content-Type"] = "application/json";
  const res = await fetch(path, { method, headers, body: body ? JSON.stringify(body) : undefined });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || ("HTTP " + res.status));
  return data;
}

function showLoggedIn(username, accountId) {
  loginSection.classList.add("hidden");
  catalogSection.classList.remove("hidden");
  accountBar.classList.remove("hidden");
  accountUsernameEl.textContent = username;
  accountIdEl.textContent = accountId;
}

function showLoggedOut() {
  loginSection.classList.remove("hidden");
  catalogSection.classList.add("hidden");
  accountBar.classList.add("hidden");
}

async function tryResumeSession() {
  if (!getToken()) {
    showLoggedOut();
    return;
  }
  try {
    const me = await api("/api/me");
    showLoggedIn(me.username, me.accountId);
    await loadCatalog();
  } catch (e) {
    setToken(null);
    showLoggedOut();
  }
}

loginForm.addEventListener("submit", async (ev) => {
  ev.preventDefault();
  loginError.classList.add("hidden");
  const username = document.getElementById("login-username").value;
  const password = document.getElementById("login-password").value;
  try {
    const result = await api("/api/login", { method: "POST", body: { username, password } });
    setToken(result.token);
    showLoggedIn(result.username, result.accountId);
    await loadCatalog();
  } catch (e) {
    loginError.textContent = e.message;
    loginError.classList.remove("hidden");
  }
});

document.getElementById("logout-btn").addEventListener("click", () => {
  setToken(null);
  showLoggedOut();
});

async function loadCatalog() {
  const { items } = await api("/api/catalog");
  catalogGrid.innerHTML = "";
  for (const item of items) {
    const card = document.createElement("div");
    card.className = "item-card";
    card.innerHTML = `
      <div class="item-kind">${item.kind}</div>
      <div class="item-name">${item.name}</div>
      <div class="item-price">${item.price} AO$</div>
      <button class="buy-btn" data-id="${item.id}">Buy</button>
    `;
    catalogGrid.appendChild(card);
  }
  catalogGrid.querySelectorAll(".buy-btn").forEach((btn) => {
    btn.addEventListener("click", () => buyItem(btn.dataset.id, btn));
  });
}

async function buyItem(catalogId, btn) {
  purchaseMessage.classList.add("hidden");
  btn.disabled = true;
  try {
    const result = await api("/api/purchase", { method: "POST", body: { catalogId } });
    purchaseMessage.textContent = result.message;
    purchaseMessage.classList.remove("error");
    purchaseMessage.classList.remove("hidden");
  } catch (e) {
    purchaseMessage.textContent = e.message;
    purchaseMessage.classList.add("error");
    purchaseMessage.classList.remove("hidden");
  } finally {
    btn.disabled = false;
  }
}

tryResumeSession();
