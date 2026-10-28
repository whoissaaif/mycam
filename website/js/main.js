(function () {
  var PAGE_ORDER = ["index", "features", "how-it-works", "download", "faq", "about"];

  function pageKey(path) {
    var p = (path || "").replace(/^\//, "").replace(/\.html$/, "").replace(/\/$/, "");
    return p === "" ? "index" : p;
  }

  document.addEventListener("click", function (event) {
    var link = event.target.closest("a[href]");
    if (!link) return;

    var href = link.getAttribute("href");
    if (!href || href.indexOf("http") === 0 || href.indexOf("#") === 0 || link.target === "_blank") {
      return;
    }

    var curIdx = PAGE_ORDER.indexOf(pageKey(window.location.pathname));
    var tgtIdx = PAGE_ORDER.indexOf(pageKey(href));
    if (curIdx === -1 || tgtIdx === -1 || curIdx === tgtIdx) return;

    sessionStorage.setItem("mycamDir", tgtIdx < curIdx ? "right" : "left");
  });
})();

document.addEventListener("DOMContentLoaded", function () {
  var toggle = document.querySelector(".nav-toggle");
  var navBar = document.querySelector(".nav-bar");

  if (toggle && navBar) {
    toggle.addEventListener("click", function () {
      navBar.classList.toggle("open");
    });
  }

  document.querySelectorAll(".faq-item").forEach(function (item) {
    var question = item.querySelector(".faq-question");
    question.addEventListener("click", function () {
      var isOpen = item.classList.contains("open");
      document.querySelectorAll(".faq-item.open").forEach(function (open) {
        open.classList.remove("open");
      });
      if (!isOpen) {
        item.classList.add("open");
      }
    });
  });
});
