/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Pixmap.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "ImageReader.h"
#include "Theme.h"
#include "SvgIcons.h"

// https://lucide.dev/icons/house
const char* gIconHome =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8"/><path d="M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>)";

// https://lucide.dev/icons/library
const char* gIconLibrary =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="m16 6 4 14"/><path d="M12 6v14"/><path d="M8 8v12"/><path d="M4 4v16"/></svg>)";

// 24gl-bookmarks2: stacked bookmark. fill uses currentColor so the toolbar
// theme can recolor it (GetCachedPixmapForSvg replaces currentColor).
const char* gIconBookmarks =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M885.333333 42.666667H309.333333a53.393333 53.393333 0 0 0-53.333333 53.333333v32H181.333333a53.393333 53.393333 0 0 0-53.333333 53.333333v778.666667a21.333333 21.333333 0 0 0 32.666667 18.093333l330-206.266666 330 206.266666a21.333333 21.333333 0 0 0 32.666666-18.093333v-142.84l52.666667 32.933333a21.333333 21.333333 0 0 0 32.666667-18.093333V96a53.393333 53.393333 0 0 0-53.333334-53.333333z m-74.666666 878.84l-308.666667-192.933334a21.333333 21.333333 0 0 0-22.613333 0l-308.666667 192.933334V181.333333a10.666667 10.666667 0 0 1 10.666667-10.666666h618.666666a10.666667 10.666667 0 0 1 10.666667 10.666666z m85.333333-128l-42.666667-26.666667V181.333333a53.393333 53.393333 0 0 0-53.333333-53.333333H298.666667v-32a10.666667 10.666667 0 0 1 10.666666-10.666667h576a10.666667 10.666667 0 0 1 10.666667 10.666667z"/></svg>)";

// https://lucide.dev/icons/plus
const char* gIconPlus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>)";

// chat bubble (toolbar / AI sidebar toggle)
const char* gIconChat =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M512 64c259.2 0 469.333333 200.576 469.333333 448s-210.133333 448-469.333333 448a484.48 484.48 0 0 1-232.725333-58.88l-116.394667 50.645333a42.666667 42.666667 0 0 1-58.517333-49.002666l29.76-125.013334C76.629333 703.402667 42.666667 611.477333 42.666667 512 42.666667 264.576 252.8 64 512 64z m0 64C287.488 128 106.666667 300.586667 106.666667 512c0 79.573333 25.557333 155.434667 72.554666 219.285333l5.525334 7.317334 18.709333 24.192-26.965333 113.237333 105.984-46.08 27.477333 15.018667C370.858667 878.229333 439.978667 896 512 896c224.512 0 405.333333-172.586667 405.333333-384S736.512 128 512 128z m-157.696 341.333333a42.666667 42.666667 0 1 1 0 85.333334 42.666667 42.666667 0 0 1 0-85.333334z m159.018667 0a42.666667 42.666667 0 1 1 0 85.333334 42.666667 42.666667 0 0 1 0-85.333334z m158.997333 0a42.666667 42.666667 0 1 1 0 85.333334 42.666667 42.666667 0 0 1 0-85.333334z"/></svg>)";

// From user asset 目标.svg — concentric target + cursor; "chat with this PDF only"
const char* gIconTargetFocus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M497.92 940.8c-235.776 0-427.52-191.744-427.52-427.52s191.744-427.52 427.52-427.52c29.952 0 59.904 3.072 89.088 9.216 16.64 3.584 27.136 19.712 23.808 36.352-3.584 16.64-19.712 27.136-36.352 23.808-24.832-5.376-50.688-7.936-76.288-7.936-201.984 0-366.08 164.096-366.08 366.08s164.096 366.08 366.08 366.08 366.08-164.096 366.08-366.08c0-23.808-2.304-47.616-6.912-70.912-3.328-16.64 7.68-32.768 24.32-36.096 16.64-3.328 32.768 7.68 36.096 24.32 5.376 27.136 7.936 54.784 7.936 82.688-0.256 235.52-192.256 427.52-427.776 427.52z"/><path d="M497.92 748.8c-129.792 0-235.52-105.728-235.52-235.52s105.728-235.52 235.52-235.52c20.736 0 41.472 2.816 61.44 8.192 16.384 4.352 26.112 21.248 21.76 37.632s-21.248 26.112-37.632 21.76c-14.848-3.84-29.952-5.888-45.312-5.888-96 0-174.08 78.08-174.08 174.08s78.08 174.08 174.08 174.08 174.08-78.08 174.08-174.08c0-10.24-1.024-20.48-2.56-30.464-3.072-16.64 8.192-32.768 24.832-35.584 16.64-3.072 32.768 8.192 35.584 24.832 2.304 13.568 3.584 27.392 3.584 41.216-0.256 129.536-105.984 235.264-235.776 235.264z"/><path d="M535.552 507.904c-8.448 0-16.896-3.584-23.04-10.24-11.264-12.8-10.24-32 2.56-43.264l157.184-139.776c12.8-11.264 32-10.24 43.264 2.56 11.264 12.8 10.24 32-2.56 43.264l-157.184 139.776c-5.632 5.12-12.8 7.68-20.224 7.68z"/><path d="M814.08 388.608c-2.304 0-4.864-0.256-7.168-0.768l-112.384-27.392c-10.496-2.56-18.944-10.496-22.016-20.736l-35.584-115.2c-3.328-11.008-0.512-22.784 7.68-30.72l121.088-121.088c7.424-7.424 18.432-10.496 28.672-8.192 10.24 2.56 18.688 9.984 22.016 20.224L848.128 179.2 942.08 210.432c9.984 3.328 17.664 11.776 20.224 22.016s-0.768 21.248-8.192 28.672l-118.272 118.272c-5.888 5.888-13.824 8.96-21.76 9.216zM725.76 304.64l78.848 19.2L875.52 252.928l-61.952-20.736c-9.216-3.072-16.384-10.24-19.456-19.456l-20.736-61.952-72.96 72.704 25.344 81.152z"/></svg>)";

// From user asset notebooklm.svg — NotebookLM tab for current PDF
const char* gIconNotebookLm =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M511.96 128C229.2 128 0 360.448 0 647.336V896h94.376v-24.744c0-116.4 92.928-210.688 207.616-210.688 114.688 0 207.616 94.288 207.616 210.64V896H604v-24.744c0-169.264-135.256-306.352-302-306.352a297.216 297.216 0 0 0-174.336 56.24c51.544-103.936 157.656-175.28 280.24-175.28 173.096 0 313.472 142.424 313.472 318V896h94.376V763.864c0-228.48-182.616-413.784-407.896-413.784a402.68 402.68 0 0 0-265.256 99.496C212.648 315.224 351.744 223.704 512 223.704c230.656 0 417.624 189.696 417.624 423.68V896H1024V647.336C1023.96 360.448 794.752 128 511.96 128z"/></svg>)";

// From user asset tab页.svg — browser tab glyph for AI WebPanel tab list
const char* gIconTabs =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M908.8 1005.44H115.2a101.76 101.76 0 0 1-101.12-101.76V110.72A101.76 101.76 0 0 1 115.2 8.96h296.96a32.64 32.64 0 0 1 32 32V262.4a32 32 0 0 1-32 32 32 32 0 0 1-32-32v-192H115.2a37.76 37.76 0 0 0-37.12 37.76v795.52a37.76 37.76 0 0 0 37.12 37.76h793.6a37.76 37.76 0 0 0 37.12-37.76V267.52a32 32 0 0 1 32-32 32 32 0 0 1 32 32v636.16a101.76 101.76 0 0 1-101.12 101.76z"/><path d="M977.92 299.52a32.64 32.64 0 0 1-32-32V180.48a37.12 37.12 0 0 0-37.12-37.76H421.12a32 32 0 0 1-32-32 32 32 0 0 1 32-32h487.68a101.76 101.76 0 0 1 101.12 101.76v87.04a32 32 0 0 1-32 32z"/><path d="M977.92 299.52H64a32 32 0 0 1-32-32 32 32 0 0 1 32-32h913.92a32 32 0 0 1 32 32 32 32 0 0 1-32 32z"/><path d="M699.52 299.52a32 32 0 0 1-32-32V110.72a32 32 0 0 1 64 0v156.8a32 32 0 0 1-32 32z"/></svg>)";

// From user asset 历史.svg — Chrome-like browsing history
const char* gIconHistory =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M511.73808594 93.19941406c-140.67070313 0-264.92695313 69.1453125-341.01035156 175.08955078v-70.47597656c0-21.66767578-17.61679688-39.22998047-39.346875-39.22998047s-39.346875 17.56318359-39.346875 39.22998047v156.91992188c0 21.66240234 17.61679688 39.22998047 39.346875 39.22998047h170.50341796c21.73007813 0 39.346875-17.56669922 39.346875-39.22998047 0-21.66767578-17.61679688-39.22998047-39.346875-39.22998047h-68.4421875c61.73085937-86.98798828 163.28496094-143.84267578 278.296875-143.84267578 188.32851562 0 341.00507812 152.22128906 341.00507813 339.99345703 0 187.76865234-152.6765625 339.99345703-341.00507813 339.99345703-158.73574219 0-291.74589844-108.29179688-329.75771484-254.70439453l-0.68291016 0.21269531c-5.3015625-15.41162109-19.54248047-26.65986328-36.8024414-26.65986328-21.73007813 0-39.346875 17.56318359-39.346875 39.22558594 0 3.86630859 1.16367187 7.36875 2.20605469 10.89316406l-0.40957032 0.12919922c0.25839844 0.95009766 0.73212891 1.8 0.99580078 2.74482422 0.28916016 0.76201172 0.48164063 1.52314453 0.81650391 2.2640625 49.92539062 175.47011719 210.98935547 304.35380859 402.98378906 304.35380859 231.79394531 0 419.69970703-187.35029297 419.69970703-418.45341797-0.00175781-231.10751953-187.90839844-418.45429688-419.70322265-418.45429687z m0 176.53183594c-21.73007813 0-39.346875 17.56318359-39.346875 39.22998047v235.37988281c0 21.66767578 17.61679688 39.22998047 39.346875 39.22998047h170.50253906c21.73007813 0 39.346875-17.56318359 39.346875-39.22558594 0-21.67119141-17.61679688-39.22998047-39.346875-39.22998047H551.08496094v-196.15429687c0-21.66767578-17.61767578-39.22998047-39.34775391-39.22998047z"/></svg>)";

// From user asset 插件.svg — puzzle piece for Chrome-like extensions menu
const char* gIconExtensions =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024" fill="currentColor"><path d="M710.234353 1023.216941H91.557647c-50.597647 0-91.497412-40.357647-91.497412-90.23247v-149.564236c0-9.517176 4.818824-16.624941 12.047059-23.732706a23.250824 23.250824 0 0 1 24.094118-4.758588c14.456471 4.758588 24.094118 4.758588 36.141176 4.758588 74.571294 0 134.746353-59.331765 134.746353-132.939294S146.853647 493.808941 72.282353 493.808941c-9.637647 0-21.684706 2.409412-36.141177 4.758588a28.310588 28.310588 0 0 1-24.094117-4.818823A29.093647 29.093647 0 0 1 0 470.136471V322.861176c0-49.874824 40.96-90.232471 91.497412-90.23247h120.350117c-2.409412-14.215529-2.409412-28.491294-2.409411-42.706824C209.438118 85.473882 296.176941 0 402.070588 0c105.953882 0 192.632471 85.473882 192.632471 189.921882 0 14.215529-2.409412 28.491294-4.818824 42.767059h120.350118c50.597647 0 91.497412 40.357647 91.497412 90.172235v116.314353c9.637647-2.409412 19.275294-2.409412 28.912941-2.409411 105.893647 0 192.572235 85.534118 192.572235 189.982117s-86.678588 189.921882-192.572235 189.921883c-9.637647 0-19.275294 0-28.912941-2.409412v116.374588c2.409412 52.224-38.550588 92.581647-91.497412 92.581647zM57.825882 819.019294v113.965177c0 18.974118 14.456471 33.249882 33.731765 33.249882H710.174118c19.275294 0 33.731765-14.275765 33.731764-33.249882v-154.322824c0-9.456941 4.818824-18.974118 12.047059-23.732706a26.142118 26.142118 0 0 1 26.50353-2.409412c16.805647 4.818824 31.262118 9.517176 45.718588 9.517177 74.631529 0 134.806588-59.331765 134.806588-132.939294 0-73.547294-60.235294-132.939294-134.806588-132.939294-14.456471 0-31.322353 2.409412-45.778824 9.517176-9.637647 2.409412-19.275294 2.409412-26.503529-2.409412a29.093647 29.093647 0 0 1-12.047059-23.732706v-156.611764a32.707765 32.707765 0 0 0-33.671529-33.310118H548.924235a26.503529 26.503529 0 0 1-24.094117-14.215529 39.152941 39.152941 0 0 1-2.409412-28.491294c9.637647-18.974118 14.456471-37.948235 14.45647-56.982589 0-73.607529-60.175059-132.939294-134.806588-132.939294-74.691765 0-134.806588 59.331765-134.806588 132.939294 0 18.974118 4.818824 37.948235 14.456471 56.982589 4.818824 9.517176 4.818824 18.974118-2.409412 28.491294a26.503529 26.503529 0 0 1-24.094118 14.215529H91.497412c-19.275294 0-33.731765 14.275765-33.731765 33.249882v113.965177h14.456471c105.953882 0 192.632471 85.473882 192.63247 189.921882s-86.738824 189.921882-192.63247 189.921883c-4.818824 2.409412-9.637647 2.409412-14.456471 2.409411z"/></svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/folder.svg
const char* gIconFileOpen =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M5 4h4l3 3h7a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-11a2 2 0 0 1 2 -2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/printer.svg
const char* gIconPrint =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M17 17h2a2 2 0 0 0 2 -2v-4a2 2 0 0 0 -2 -2h-14a2 2 0 0 0 -2 2v4a2 2 0 0 0 2 2h2" />
  <path d="M17 9v-4a2 2 0 0 0 -2 -2h-6a2 2 0 0 0 -2 2v4" />
  <rect x="7" y="13" width="10" height="8" rx="2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-left.svg
const char* gIconPagePrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="5" y1="12" x2="19" y2="12" />
  <line x1="5" y1="12" x2="11" y2="18" />
  <line x1="5" y1="12" x2="11" y2="6" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-right.svg
const char* gIconPageNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="5" y1="12" x2="19" y2="12" />
  <line x1="13" y1="18" x2="19" y2="12" />
  <line x1="13" y1="6" x2="19" y2="12" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/layout-rows.svg
const char* gIconLayoutContinuous =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="3" y="3" width="18" height="18" rx="2" />
  <line x1="3" y1="12" x2="21" y2="12" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/square.svg
const char* gIconLayoutSinglePage =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="4" y="4" width="16" height="16" rx="2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-left.svg
const char* gIconSearchPrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="15 6 9 12 15 18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-right.svg
const char* gIconSearchNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="9 6 15 12 9 18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/letter-case.svg
const char* gIconMatchCase =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <circle cx="18" cy="16" r="3" />
  <line x1="21" y1="13" x2="21" y2="19" />
  <path d="M3 19l5 -13l5 13" />
  <line x1="5" y1="14" x2="11" y2="14" />
</svg>)";

// "match whole word": lowercase "ab" over an underline bracketed at both ends,
// suggesting a complete word delimited by word boundaries (like VS Code's
// whole-word toggle). Custom icon drawn in the tabler stroke style.
const char* gIconMatchWholeWord =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <circle cx="7" cy="11" r="2.5" />
  <line x1="9.5" y1="8.5" x2="9.5" y2="13.5" />
  <line x1="14.5" y1="6" x2="14.5" y2="13.5" />
  <circle cx="17" cy="11" r="2.5" />
  <path d="M3 16v3h18v-3" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/zoom-in.svg
const char* gIconZoomIn =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="7" y1="10" x2="13" y2="10" />
  <line x1="10" y1="7" x2="10" y2="13" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/zoom-out.svg
const char* gIconZoomOut =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="7" y1="10" x2="13" y2="10" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/floppy-disk.svg
const char* gIconSave =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <path d="M18 20h-12a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2h9l5 5v9a2 2 0 0 1 -2 2" />
  <circle cx="12" cy="13" r="2" />
  <polyline points="4 8 10 8 10 4" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/rotate-2.svg - modified
const char* gIconRotateLeft =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M15 4.55a8 8 0 0 0 -6 14.9m0 -5.45v6h-6"/>
  <circle cx="18.37" cy="7.16" r="0.15"/>
  <circle cx="13" cy="19.94" r="0.15"/>
  <circle cx="16.84" cy="18.37" r="0.15"/>
  <circle cx="19.37" cy="15.1" r="0.15"/>
  <circle cx="19.94" cy="11" r="0.15"/>
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/rotate-clockwise-2.svg - modified
const char* gIconRotateRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M9 4.55a8 8 0 0 1 6 14.9m0 -5.45v6h6"/>
  <circle cx="5.63" cy="7.16" r="0.15"/>
  <circle cx="4.06" cy="11" r="0.15"/>
  <circle cx="4.63" cy="15.1" r="0.15"/>
  <circle cx="7.16" cy="18.37" r="0.15"/>
  <circle cx="11" cy="19.94" r="0.15"/>
</svg>)";

const char* gIconSpeak =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M15 8a5 5 0 0 1 0 8" />
  <path d="M17.7 5a9 9 0 0 1 0 14" />
  <path d="M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5" />
</svg>)";

// tabler player-pause
const char* gIconPauseSpeaking =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M6 5v14" />
  <path d="M18 5v14" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-back-up.svg
const char* gIconNavigateBack =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M9 14l-4 -4l4 -4" />
  <path d="M5 10h11a4 4 0 1 1 0 8h-1" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-forward-up.svg
const char* gIconNavigateForward =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M15 14l4 -4l-4 -4" />
  <path d="M19 10h-11a4 4 0 1 0 0 8h1" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/search.svg
const char* gIconSearch =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-up.svg
const char* gIconChevronUp =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="6 15 12 9 18 15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-down.svg
const char* gIconChevronDown =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="6 9 12 15 18 9" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/x.svg
const char* gIconClose =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="18" y1="6" x2="6" y2="18" />
  <line x1="6" y1="6" x2="18" y2="18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/pin.svg
// tabler arrows-diagonal: expand the compact find bar into a floating window
const char* gIconArrowsDiagonal =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M16 4l4 0l0 4" />
  <path d="M14 10l6 -6" />
  <path d="M8 20l-4 0l0 -4" />
  <path d="M4 20l6 -6" />
</svg>)";

// tabler arrows-diagonal-minimize-2: dock the floating window back to the bar
const char* gIconArrowsDiagonalMinimize =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M18 10l-4 0l0 -4" />
  <path d="M20 4l-6 6" />
  <path d="M6 14l4 0l0 4" />
  <path d="M10 14l-6 6" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/list.svg
const char* gIconHomeList =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="9" y1="6" x2="20" y2="6" />
  <line x1="9" y1="12" x2="20" y2="12" />
  <line x1="9" y1="18" x2="20" y2="18" />
  <line x1="5" y1="6" x2="5" y2="6.01" />
  <line x1="5" y1="12" x2="5" y2="12.01" />
  <line x1="5" y1="18" x2="5" y2="18.01" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/layout-grid.svg
const char* gIconHomeThumbnails =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="4" y="4" width="6" height="6" rx="1" />
  <rect x="14" y="4" width="6" height="6" rx="1" />
  <rect x="4" y="14" width="6" height="6" rx="1" />
  <rect x="14" y="14" width="6" height="6" rx="1" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/pin.svg
const char* gIconPin =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M15 4.5l4.5 4.5" />
  <path d="M14.5 9.5l-5 5" />
  <path d="M9 15l-4 4" />
  <path d="M9.5 4l10.5 10.5l-5.5 0.5l-4 4l-1 -4.5l-4.5 -1l4 -4z" />
</svg>)";

// A custom ToolbarSvgIcon comes from the settings file, so it can be malformed:
// a typo, or the file caught half-written by the settings watcher while the user
// is editing it. mupdf signals that by throwing, and an uncaught mupdf exception
// aborts the whole process, so everything here has to be inside fz_try.
static fz_pixmap* RenderSvgToFzPixmap(fz_context* ctx, Str svgData, int dx, int dy, Color fgCol, Color bgCol) {
    TempStr strokeCol = SerializeColorTemp(fgCol);
    TempStr fillCol = SerializeColorTemp(bgCol);
    TempStr fillColRepl = str::JoinTemp(StrL("fill=\""), fillCol, StrL("\""));
    TempStr svg = str::ReplaceTemp(svgData, StrL("currentColor"), strokeCol);
    svg = str::ReplaceTemp(svg, StrL(R"(fill="none")"), fillColRepl);

    fz_buffer* buf = nullptr;
    fz_image* image = nullptr;
    fz_pixmap* pixmap = nullptr;
    fz_var(buf);
    fz_var(image);
    fz_var(pixmap);
    fz_try(ctx) {
        buf = fz_new_buffer_from_copied_data(ctx, (u8*)svg.s, svg.len);
        image = fz_new_image_from_svg(ctx, buf, nullptr, nullptr);
        image->w = dx;
        image->h = dy;
        pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    fz_always(ctx) {
        fz_drop_image(ctx, image);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("GetCachedPixmapForSvg: rendering svg icon failed with: '%s'\n", Str(fz_caught_message(ctx)));
        return nullptr;
    }
    return pixmap;
}

static void BlitFzPixmapBgra(u8* dstSamples, ptrdiff_t dstStride, fz_pixmap* src, Color bgCol) {
    int dx = src->w;
    int dy = src->h;
    int srcN = src->n;
    auto srcStride = src->stride;
    u8 r, g, b;
    UnpackColor(bgCol, r, g, b);
    for (size_t y = 0; y < (size_t)dy; y++) {
        u8* s = src->samples + (srcStride * y);
        u8* d = dstSamples + (dstStride * y);
        for (int x = 0; x < dx; x++) {
            bool isTransparent = (s[0] == r) && (s[1] == g) && (s[2] == b);
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = isTransparent ? 0 : 0xff;
            d += 4;
            s += srcN;
        }
    }
}

// BGRA DIB, alpha-premultiplied, transparent where the SVG left the background.
static Pixmap* RenderSvgToPixmap(Str svgData, int dx, int dy, Color fgCol, Color bgCol) {
    Pixmap* px = AllocPixmapDIB(dx, dy);
    if (!px) {
        return nullptr;
    }
    memset(px->data, 0, (size_t)px->stride * (size_t)dy);
    px->premultiplied = true;

    fz_context* ctx = fz_new_context_windows();
    fz_pixmap* pixmap = RenderSvgToFzPixmap(ctx, svgData, dx, dy, fgCol, bgCol);
    if (pixmap) {
        BlitFzPixmapBgra(px->data, px->stride, pixmap, bgCol);
        u8* row = px->data;
        for (int y = 0; y < dy; y++) {
            u8* d = row;
            for (int x = 0; x < dx; x++) {
                if (d[3] == 0) {
                    d[0] = d[1] = d[2] = 0;
                }
                d += 4;
            }
            row += px->stride;
        }
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_drop_context_windows(ctx);
    return px;
}

// Super-set of the old GetPixmapForIcon / SelToolbarIcon caches: keyed by
// SVG bytes (built-in gIcon* or a user-provided string), size, and colors.
struct SvgPixmapCacheEntry {
    SvgPixmapCacheEntry* next = nullptr;
    Str svg; // owned
    int dx = 0;
    int dy = 0;
    Color fg = 0;
    Color bg = 0;
    Pixmap* pixmap = nullptr; // owned

    ~SvgPixmapCacheEntry() {
        str::Free(svg);
        FreePixmap(pixmap);
    }
};

static SvgPixmapCacheEntry* gSvgPixmapCache = nullptr;

// Render `svg` at dx×dy in fg/bg (theme text/control colors if unset).
// The Pixmap belongs to the cache until DestroySvgPixmapIconsCache().
Pixmap* GetCachedPixmapForSvg(Str svg, int dx, int dy, Color fg, Color bg) {
    if (str::IsEmptyOrWhiteSpace(svg) || dx <= 0 || dy <= 0) {
        return nullptr;
    }
    if (fg == kColorUnset) {
        fg = ThemeWindowTextColor();
    }
    if (bg == kColorUnset) {
        bg = ThemeControlBackgroundColor();
    }
    for (SvgPixmapCacheEntry* e = gSvgPixmapCache; e; e = e->next) {
        if (e->dx == dx && e->dy == dy && e->fg == fg && e->bg == bg && str::Eq(e->svg, svg)) {
            return e->pixmap;
        }
    }
    Pixmap* px = RenderSvgToPixmap(svg, dx, dy, fg, bg);
    if (!px) {
        return nullptr;
    }
    auto* e = new SvgPixmapCacheEntry();
    e->svg = str::Dup(svg);
    e->dx = dx;
    e->dy = dy;
    e->fg = fg;
    e->bg = bg;
    e->pixmap = px;
    ListInsertFront(&gSvgPixmapCache, e);
    return px;
}

// Pixmaps from earlier theme/DPI generations. Controls hold raw Pixmap* from
// GetCachedPixmapForSvg() and not all of them are refreshed on theme/DPI change,
// so freeing immediately left dangling pointers (AV in PixmapAsPremulBgra while
// painting a header button). Icons are tiny; keep them until shutdown.
static SvgPixmapCacheEntry* gRetiredSvgPixmaps = nullptr;

static void RetireSvgPixmapCache() {
    SvgPixmapCacheEntry* e = gSvgPixmapCache;
    while (e) {
        SvgPixmapCacheEntry* next = e->next;
        e->next = gRetiredSvgPixmaps;
        gRetiredSvgPixmaps = e;
        e = next;
    }
    gSvgPixmapCache = nullptr;
}

// Theme and DPI: subsequent lookups render in the current colors/size.
void DestroySvgPixmapIconsCache() {
    RetireSvgPixmapCache();
}

void FreeSvgPixmapIconsCacheAtShutdown() {
    ListDelete(gSvgPixmapCache);
    gSvgPixmapCache = nullptr;
    ListDelete(gRetiredSvgPixmaps);
    gRetiredSvgPixmaps = nullptr;
}
