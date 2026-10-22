// Shared viewer for the design docs: sidebar of pages, search, edit in place, save.
//
// A doc is a plain HTML file: <body data-doc="..."><main> holding one
// <section class="page" id="..." data-group="..."> per page, then this script.
// Everything this script adds carries data-ui and is stripped again on save, so
// the saved file has the same shape as the hand-written one (see serialize()).
(function () {
  "use strict";

  var body = document.body;
  var main = document.querySelector("main");
  var DOC = body.getAttribute("data-doc") || "doc";
  var FILE = decodeURIComponent(location.pathname.split("/").pop() || DOC + ".html");
  var DRAFT_KEY = "xxl-design-draft:" + DOC;

  var state = { cur: null, editing: false, dirty: false, query: "" };
  var bar, nav, tools, search, dirtyDot, editBtn, saveBtn, toast, draftTimer;

  // --- small helpers --------------------------------------------------------
  function el(tag, attrs, html) {
    var e = document.createElement(tag);
    e.setAttribute("data-ui", "");
    for (var k in attrs || {}) e.setAttribute(k, attrs[k]);
    if (html != null) e.innerHTML = html;
    return e;
  }
  function esc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }
  function pages() {
    return Array.prototype.filter.call(main.children, function (c) {
      return c.tagName === "SECTION" && c.classList.contains("page");
    });
  }
  function titleOf(p) {
    var h = p.querySelector("h1");
    return (h ? h.textContent : p.id).trim() || p.id;
  }
  function groupOf(p) { return p.getAttribute("data-group") || "Pages"; }
  function groups() {
    var out = [];
    pages().forEach(function (p) { if (out.indexOf(groupOf(p)) < 0) out.push(groupOf(p)); });
    return out;
  }
  function slug(s) {
    var base = s.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "page";
    var id = base, n = 2;
    while (document.getElementById(id)) id = base + "-" + n++;
    return id;
  }
  function store(fn) { try { return fn(window.localStorage); } catch (e) { return null; } }
  function say(msg) {
    toast.textContent = msg;
    toast.classList.add("on");
    clearTimeout(say.t);
    say.t = setTimeout(function () { toast.classList.remove("on"); }, 2600);
  }
  // Text of a page without the viewer's own additions (for search).
  function plainText(p) {
    var c = p.cloneNode(true);
    Array.prototype.forEach.call(c.querySelectorAll("[data-ui]"), function (e) { e.remove(); });
    return c.textContent.replace(/\s+/g, " ");
  }

  // --- serialization (save, draft, selftest) --------------------------------
  function attrs(e) {
    return Array.prototype.map.call(e.attributes, function (a) {
      return " " + a.name + '="' + a.value.replace(/&/g, "&amp;").replace(/"/g, "&quot;") + '"';
    }).join("");
  }
  function clean(root) {
    Array.prototype.forEach.call(root.querySelectorAll("[data-ui]"), function (e) { e.remove(); });
    Array.prototype.forEach.call(root.querySelectorAll("mark.hl"), function (m) {
      while (m.firstChild) m.parentNode.insertBefore(m.firstChild, m);
      m.remove();
    });
    Array.prototype.forEach.call(root.querySelectorAll("[contenteditable]"), function (e) {
      e.removeAttribute("contenteditable");
    });
    var all = [root].concat(Array.prototype.slice.call(root.querySelectorAll("[class]")));
    all.forEach(function (e) {
      if (!e.classList) return;
      e.classList.remove("cur", "editing", "nav-open");
      if (e.hasAttribute("class") && !e.getAttribute("class").trim()) e.removeAttribute("class");
    });
    root.normalize();
    return root;
  }
  // The pages as they go into the file: one blank line between sections.
  function mainHtml(m) {
    var secs = Array.prototype.filter.call(m.children, function (c) { return c.tagName === "SECTION"; });
    return "\n" + secs.map(function (s) { return s.outerHTML; }).join("\n\n") + "\n";
  }
  function serialize() {
    var root = clean(document.documentElement.cloneNode(true));
    var head = root.querySelector("head"), b = root.querySelector("body"), m = b.querySelector("main");
    var t = head.querySelector("title");
    if (t) t.textContent = baseTitle;  // the tab title shows the open page; the file keeps the doc's
    m.innerHTML = mainHtml(m);
    var inner = b.innerHTML.replace(/^\s+|\s+$/g, "");
    return "<!doctype html>\n<html" + attrs(root) + ">\n" + head.outerHTML + "\n<body" + attrs(b) + ">\n" +
      inner + "\n</body>\n</html>\n";
  }
  function serializeMain() {
    var m = clean(main.cloneNode(true));
    return mainHtml(m);
  }

  // --- dirty state and drafts -----------------------------------------------
  function setDirty(on) {
    state.dirty = on;
    dirtyDot.classList.toggle("on", on);
    dirtyDot.title = on ? "Unsaved changes" : "";
    if (on) {
      clearTimeout(draftTimer);
      draftTimer = setTimeout(function () {
        var d = JSON.stringify({ t: Date.now(), main: serializeMain() });
        store(function (s) { s.setItem(DRAFT_KEY, d); });
      }, 800);
    } else {
      clearTimeout(draftTimer);
      store(function (s) { s.removeItem(DRAFT_KEY); });
    }
  }
  function offerDraft() {
    var raw = store(function (s) { return s.getItem(DRAFT_KEY); });
    if (!raw) return;
    var d;
    try { d = JSON.parse(raw); } catch (e) { return; }
    if (!d || !d.main || d.main === serializeMain()) {
      store(function (s) { s.removeItem(DRAFT_KEY); });
      return;
    }
    var when = new Date(d.t).toLocaleString();
    var banner = el("div", { "class": "vw-banner" },
      "<span>Unsaved edits from " + esc(when) + " are kept in this browser.</span>");
    var restore = el("button", { "class": "vw-btn" }, "Restore them");
    var drop = el("button", { "class": "vw-btn" }, "Discard");
    restore.onclick = function () {
      main.innerHTML = d.main;
      banner.remove();
      buildTools();
      renderNav();
      route();
      setDirty(true);
      say("Draft restored. Save to write it to the file.");
    };
    drop.onclick = function () { banner.remove(); store(function (s) { s.removeItem(DRAFT_KEY); }); };
    banner.appendChild(restore);
    banner.appendChild(drop);
    body.insertBefore(banner, nav);
  }

  // --- save -------------------------------------------------------------------
  function idb(fn) {
    return new Promise(function (resolve) {
      try {
        var req = indexedDB.open("xxl-design-docs", 1);
        req.onupgradeneeded = function () { req.result.createObjectStore("handles"); };
        req.onerror = function () { resolve(null); };
        req.onsuccess = function () {
          try {
            var tx = req.result.transaction("handles", "readwrite");
            var r = fn(tx.objectStore("handles"));
            tx.oncomplete = function () { resolve(r && "result" in r ? r.result : null); };
            tx.onerror = function () { resolve(null); };
          } catch (e) { resolve(null); }
        };
      } catch (e) { resolve(null); }
    });
  }
  function download(text) {
    var a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "text/html" }));
    a.download = FILE;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setDirty(false);
    say("Downloaded " + FILE + " — put it over the doc in coop/docs/design.");
  }
  async function save() {
    var text = serialize();
    if (!window.showSaveFilePicker) return download(text);
    try {
      var h = await idb(function (s) { return s.get(DOC); });
      if (h && (await h.queryPermission({ mode: "readwrite" })) !== "granted" &&
          (await h.requestPermission({ mode: "readwrite" })) !== "granted") h = null;
      if (!h) {
        h = await window.showSaveFilePicker({
          suggestedName: FILE,
          types: [{ description: "HTML page", accept: { "text/html": [".html"] } }]
        });
        await idb(function (s) { return s.put(h, DOC); });
      }
      var w = await h.createWritable();
      await w.write(text);
      await w.close();
      setDirty(false);
      say("Saved to " + h.name);
    } catch (e) {
      if (e && e.name === "AbortError") return;
      console.warn("save:", e);
      download(text);
    }
  }
  async function forgetFile() {
    await idb(function (s) { return s.delete(DOC); });
    say("Save will ask for the file again.");
  }

  // --- navigation ---------------------------------------------------------------
  function renderNav() {
    nav.innerHTML = "";
    var q = state.query;
    if (q.length >= 2) {
      var hits = 0;
      pages().forEach(function (p) {
        var text = plainText(p), low = text.toLowerCase(), i = low.indexOf(q);
        if (i < 0) return;
        hits++;
        var n = low.split(q).length - 1;
        var from = Math.max(0, i - 40), snip = text.slice(from, i + q.length + 80);
        var a = el("a", { "class": "vw-result", href: "#" + p.id },
          "<b>" + esc(titleOf(p)) + "</b><span>" + (from ? "…" : "") +
          esc(snip).replace(new RegExp(q.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "ig"), function (m) {
            return '<mark class="hl">' + m + "</mark>";
          }) + "… (" + n + ")</span>");
        nav.appendChild(a);
      });
      if (!hits) nav.appendChild(el("div", { "class": "vw-group" }, "No match"));
      return;
    }
    groups().forEach(function (g) {
      var head = el("div", { "class": "vw-group" }, "<span>" + esc(g) + "</span>");
      if (state.editing) {
        var add = el("button", { "class": "vw-btn vw-small", title: "New page in " + g }, "+");
        add.onclick = function () { newPage(g); };
        head.appendChild(add);
      }
      nav.appendChild(head);
      pages().filter(function (p) { return groupOf(p) === g; }).forEach(function (p) {
        var a = el("a", { "class": "vw-link" + (p === state.cur ? " cur" : ""), href: "#" + p.id }, esc(titleOf(p)));
        nav.appendChild(a);
      });
    });
  }
  function removeExtras(p) {
    Array.prototype.forEach.call(p.querySelectorAll("[data-ui]"), function (e) { e.remove(); });
  }
  function addExtras(p) {
    removeExtras(p);
    if (state.editing) return;
    var h2 = p.querySelectorAll("h2");
    if (h2.length >= 3) {
      var toc = el("div", { "class": "vw-toc" }, "<b>On this page</b>");
      Array.prototype.forEach.call(h2, function (h) {
        var a = el("a", { href: "#" + p.id }, esc(h.textContent));
        a.onclick = function (ev) { ev.preventDefault(); h.scrollIntoView({ behavior: "smooth", block: "start" }); };
        toc.appendChild(a);
      });
      var after = p.querySelector(".meta") || p.querySelector("h1");
      if (after) after.parentNode.insertBefore(toc, after.nextSibling); else p.insertBefore(toc, p.firstChild);
    }
    var all = pages(), i = all.indexOf(p), pager = el("div", { "class": "vw-pager" });
    pager.innerHTML = (i > 0 ? '<a href="#' + all[i - 1].id + '">← ' + esc(titleOf(all[i - 1])) + "</a>" : "<span></span>") +
      (i < all.length - 1 ? '<a href="#' + all[i + 1].id + '">' + esc(titleOf(all[i + 1])) + " →</a>" : "<span></span>");
    p.appendChild(pager);
  }
  function clearHighlights() {
    Array.prototype.forEach.call(main.querySelectorAll("mark.hl"), function (m) {
      var parent = m.parentNode;
      while (m.firstChild) parent.insertBefore(m.firstChild, m);
      m.remove();
      parent.normalize();
    });
  }
  function highlight(p, q) {
    clearHighlights();
    if (!q || q.length < 2 || state.editing) return null;
    var walker = document.createTreeWalker(p, NodeFilter.SHOW_TEXT, {
      acceptNode: function (n) {
        return n.parentNode.closest("[data-ui]") ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
      }
    });
    var nodes = [], n, first = null;
    while ((n = walker.nextNode())) if (n.nodeValue.toLowerCase().indexOf(q) >= 0) nodes.push(n);
    nodes.slice(0, 300).forEach(function (t) {
      var low = t.nodeValue.toLowerCase(), i;
      while ((i = low.indexOf(q)) >= 0) {
        var hit = t.splitText(i), rest = hit.splitText(q.length);
        var m = document.createElement("mark");
        m.className = "hl";
        hit.parentNode.insertBefore(m, hit);
        m.appendChild(hit);
        if (!first) first = m;
        t = rest;
        low = t.nodeValue.toLowerCase();
      }
    });
    return first;
  }
  function show(p, target) {
    if (!p) return;
    if (state.cur && state.cur !== p) {
      state.cur.classList.remove("cur");
      state.cur.removeAttribute("contenteditable");
      removeExtras(state.cur);
    }
    state.cur = p;
    p.classList.add("cur");
    if (state.editing) p.setAttribute("contenteditable", "true");
    addExtras(p);
    renderNav();
    document.title = titleOf(p) + " · " + baseTitle;
    body.classList.remove("nav-open");
    var hit = highlight(p, state.query);
    var go = target || hit;
    if (go) go.scrollIntoView({ block: "center" }); else window.scrollTo(0, 0);
    state.target = go;
  }
  function route() {
    var id = decodeURIComponent(location.hash.slice(1));
    var t = id && document.getElementById(id);
    var p = t && (t.closest ? t.closest("section.page") : null);
    if (p && p.parentNode === main) show(p, t === p ? null : t);
    else show(pages()[0]);
  }

  // --- editing ----------------------------------------------------------------
  function setEditing(on) {
    state.editing = on;
    body.classList.toggle("editing", on);
    editBtn.classList.toggle("on", on);
    editBtn.textContent = on ? "Done" : "Edit";
    if (on) {
      clearHighlights();
      try { document.execCommand("defaultParagraphSeparator", false, "p"); } catch (e) {}
    }
    pages().forEach(function (p) { p.removeAttribute("contenteditable"); });
    renderNav();
    show(state.cur);
    if (on) state.cur.focus();
  }
  function cmd(name, arg) {
    state.cur.focus();
    document.execCommand(name, false, arg);
    setDirty(true);
  }
  function wrapCode() {
    var sel = window.getSelection();
    if (!sel.rangeCount || sel.isCollapsed) return say("Select some text first.");
    var r = sel.getRangeAt(0), c = document.createElement("code");
    c.appendChild(r.extractContents());
    r.insertNode(c);
    setDirty(true);
  }
  function insertTag() {
    var kinds = { ok: "confirmed on pad", wait: "awaiting pad test", park: "parked", claude: "decided by Claude" };
    var k = prompt("Status tag: ok, wait, park or claude", "ok");
    if (!k || !kinds[k.trim()]) return;
    cmd("insertHTML", '<span class="tag ' + k.trim() + '">' + kinds[k.trim()] + "</span> ");
  }
  function newPage(group) {
    var title = prompt("Title of the new page");
    if (!title) return;
    group = group || prompt("Group (" + groups().join(", ") + ")", groupOf(state.cur));
    if (!group) return;
    var s = document.createElement("section");
    s.className = "page";
    s.id = slug(title);
    s.setAttribute("data-group", group);
    s.innerHTML = "\n<h1>" + esc(title) + "</h1>\n<p>Write here.</p>\n";
    var inGroup = pages().filter(function (p) { return groupOf(p) === group; });
    var after = inGroup.length ? inGroup[inGroup.length - 1] : pages()[pages().length - 1];
    main.insertBefore(s, after ? after.nextSibling : null);
    setDirty(true);
    location.hash = s.id;
  }
  function movePage(dir) {
    var p = state.cur, all = pages().filter(function (x) { return groupOf(x) === groupOf(p); });
    var i = all.indexOf(p), other = all[i + dir];
    if (!other) return say("Already the " + (dir < 0 ? "first" : "last") + " page of its group.");
    main.insertBefore(p, dir < 0 ? other : other.nextSibling);
    setDirty(true);
    renderNav();
  }
  function deletePage() {
    var p = state.cur;
    if (pages().length < 2) return say("A doc keeps at least one page.");
    if (!confirm('Delete the page "' + titleOf(p) + '"?')) return;
    var all = pages(), i = all.indexOf(p), next = all[i + 1] || all[i - 1];
    p.remove();
    state.cur = null;
    setDirty(true);
    location.hash = next.id;
    show(next);
  }
  function renamePage() {
    var g = prompt("Group of this page (" + groups().join(", ") + ")", groupOf(state.cur));
    if (!g) return;
    state.cur.setAttribute("data-group", g);
    setDirty(true);
    renderNav();
  }
  function buildTools() {
    tools = el("div", { "class": "vw-tools" });
    var buttons = [
      ["P", "Paragraph", function () { cmd("formatBlock", "<p>"); }],
      ["H2", "Section heading", function () { cmd("formatBlock", "<h2>"); }],
      ["H3", "Sub-heading", function () { cmd("formatBlock", "<h3>"); }],
      ["• List", "Bullet list", function () { cmd("insertUnorderedList"); }],
      ["1. List", "Numbered list", function () { cmd("insertOrderedList"); }],
      ["B", "Bold", function () { cmd("bold"); }],
      ["I", "Italic", function () { cmd("italic"); }],
      ["Code", "Mark as code", wrapCode],
      ["Link", "Link", function () { var u = prompt("Link address"); if (u) cmd("createLink", u); }],
      ["Decision", "Design decision box", function () {
        cmd("insertHTML", '<div class="decision"><p>What was decided.</p><p>Why.</p></div><p></p>');
      }],
      ["Tag", "Status tag", insertTag],
      ["|"],
      ["New page", "New page", function () { newPage(); }],
      ["Group…", "Move this page to another group", renamePage],
      ["↑", "Move page up in its group", function () { movePage(-1); }],
      ["↓", "Move page down in its group", function () { movePage(1); }],
      ["Delete page", "Delete this page", deletePage],
      ["|"],
      ["Forget file", "Make Save ask for the file again", forgetFile]
    ];
    buttons.forEach(function (b) {
      if (b[0] === "|") { tools.appendChild(el("span", { "class": "vw-spacer" })); return; }
      var x = el("button", { "class": "vw-btn", title: b[1] }, esc(b[0]));
      x.onmousedown = function (ev) { ev.preventDefault(); };  // keep the text selection
      x.onclick = b[2];
      tools.appendChild(x);
    });
    main.insertBefore(tools, main.firstChild);
  }

  // --- start ------------------------------------------------------------------
  var baseTitle = document.title;
  bar = el("header", { "class": "vw-bar" });
  var menu = el("button", { "class": "vw-btn vw-menu", title: "Pages" }, "☰");
  menu.onclick = function () { body.classList.toggle("nav-open"); };
  bar.appendChild(menu);
  bar.appendChild(el("div", { "class": "vw-title" }, esc(baseTitle) + "<small>design doc</small>"));
  bar.appendChild(el("div", { "class": "vw-spacer" }));
  search = el("input", { "class": "vw-search", type: "search", placeholder: "Search  ( / )" });
  bar.appendChild(search);
  dirtyDot = el("span", { "class": "vw-dirty" });
  bar.appendChild(dirtyDot);
  editBtn = el("button", { "class": "vw-btn", title: "Edit this page (E)" }, "Edit");
  editBtn.onclick = function () { setEditing(!state.editing); };
  bar.appendChild(editBtn);
  saveBtn = el("button", { "class": "vw-btn", title: "Save the doc to its file (Ctrl+S)" }, "Save");
  saveBtn.onclick = save;
  bar.appendChild(saveBtn);
  nav = el("nav", { "class": "vw-nav" });
  toast = el("div", { "class": "vw-toast" });
  body.insertBefore(nav, body.firstChild);
  body.insertBefore(bar, nav);
  body.appendChild(toast);
  buildTools();

  search.addEventListener("input", function () {
    state.query = search.value.trim().toLowerCase();
    renderNav();
    if (state.cur) highlight(state.cur, state.query);
  });
  search.addEventListener("keydown", function (e) {
    if (e.key === "Enter") {
      var first = nav.querySelector("a");
      if (first) first.click();
    } else if (e.key === "Escape") {
      search.value = "";
      state.query = "";
      clearHighlights();
      renderNav();
      search.blur();
    }
  });
  main.addEventListener("input", function () { setDirty(true); });
  main.addEventListener("click", function (e) {
    var img = e.target.closest && e.target.closest("figure img");
    if (!img || state.editing) return;
    var box = el("div", { "class": "vw-lightbox" }, '<img src="' + esc(img.getAttribute("src")) + '" alt="">');
    box.onclick = function () { box.remove(); };
    body.appendChild(box);
  });
  window.addEventListener("hashchange", route);
  window.addEventListener("beforeunload", function (e) {
    if (!state.dirty) return;
    e.preventDefault();
    e.returnValue = "";
  });
  document.addEventListener("keydown", function (e) {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") {
      e.preventDefault();
      save();
      return;
    }
    var t = e.target, typing = t.isContentEditable || /INPUT|TEXTAREA|SELECT/.test(t.tagName);
    if (e.key === "Escape") {
      var box = document.querySelector(".vw-lightbox");
      if (box) box.remove();
      body.classList.remove("nav-open");
    }
    if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key === "/") { e.preventDefault(); search.focus(); }
    else if (e.key === "e" || e.key === "E") { e.preventDefault(); setEditing(!state.editing); }
  });

  pages().forEach(function (p) { if (!p.id) p.id = slug(titleOf(p)); });
  renderNav();
  route();
  offerDraft();
  // The browser's own jump to #page-id comes after load and would put the page's
  // title under the bar: start the page at its top instead.
  window.addEventListener("load", function () {
    setTimeout(function () {
      if (state.target) state.target.scrollIntoView({ block: "center" }); else window.scrollTo(0, 0);
    }, 0);
  });

  if (/[?&]selftest/.test(location.search)) {
    var out = el("pre", { id: "vw-selftest" });
    out.textContent = serialize();
    body.appendChild(out);
  }
})();
