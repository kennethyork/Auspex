#include "auspex/canvas.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <set>

#include "auspex/display.hpp"

namespace auspex {

namespace {

// Floor on a fitted tile. A window smaller than this is technically visible and
// practically worthless, so the grid is allowed to overflow the viewport instead.
// Zoom limits. Out to a third, in to three times life size.
//
// The ceiling used to be 1.00, because zooming past life size makes windows LARGER
// than the screen and the layout then spilled onto the monitor next door. Measured
// at the time: two steps of zoom-in took a 1913x1012 window to 2993x1590 and the
// following reset placed it at x=979, on the display to the left.
//
// That is no longer how oversize is handled. apply_positions caps a requested size
// at the monitor's edge and parks anything that does not belong on screen BELOW
// every monitor, so growing past the viewport can no longer reach a neighbour --
// see the containment test, which walks a canvas through every zoom step asserting
// exactly that. With the spill fixed at its source, a 1.00 ceiling only made "+" a
// button that could not do anything: at life size it re-placed every window to
// where it already was, costing two settling sleeps each and moving nothing.
//
// What zoom-in means here is still worth being precise about. X11 cannot magnify a
// window's CONTENTS -- only Auspex's own compositor will do that, through
// wlr_scene_buffer_set_dest_size. Above 1.0 a window is given more of the screen
// and its neighbours are pushed off the edge, so zooming in is how you go from
// several windows sharing the monitor to one of them having it.
constexpr double kMinZoom = 0.33;
constexpr double kMaxZoom = 3.00;

constexpr int kMinTileWidth  = 280;
constexpr int kMinTileHeight = 200;

// The narrowest a window is asked to become before the canvas gives up and parks it
// instead. Below roughly this, applications start refusing the resize and keep their
// old size -- which on a canvas that owns one monitor means hanging over onto the
// next one. Chosen to match the tile floor: a window too small to be a tile is too
// small to be worth showing.
constexpr int kMinPlacedWidth  = kMinTileWidth;
constexpr int kMinPlacedHeight = kMinTileHeight;

}  // namespace

void Canvas::set_insets(int top, int bottom) {
    inset_top_    = std::max(0, top);
    inset_bottom_ = std::max(0, bottom);
}

void Canvas::set_viewport(const Viewport& viewport) {
    viewport_ = viewport;
    if (viewport_.width  < 1) viewport_.width  = 1;
    if (viewport_.height < 1) viewport_.height = 1;
}

void Canvas::place(const std::string& window_id, CanvasPlacement placement) {
    if (window_id.empty()) return;
    placements_[window_id] = placement;
}

CanvasPlacement Canvas::place_next(const std::string& window_id, int tile_width,
                                   int tile_height) {
    if (tile_width  < 1) tile_width  = 1;
    if (tile_height < 1) tile_height = 1;

    // Usable height excludes the panels, so a tile is never placed underneath one.
    const int usable_height =
        std::max(1, viewport_.height - inset_top_ - inset_bottom_);

    const int columns = std::max(1, viewport_.width / tile_width);
    const int rows    = std::max(1, usable_height / tile_height);
    const int per_page = columns * rows;

    // Slots fill the visible page first, then continue onto the next page of canvas
    // to the right, so opening many terminals walks across the canvas instead of
    // piling them all at the origin.
    const auto slot_placement = [&](int slot) {
        const int page = per_page > 0 ? slot / per_page : 0;
        const int cell = per_page > 0 ? slot % per_page : 0;
        return CanvasPlacement{
            .x = viewport_.x + page * viewport_.width + (cell % columns) * tile_width,
            .y = viewport_.y + inset_top_ + (cell / columns) * tile_height};
    };

    // The FIRST FREE slot, not the next one in sequence. A monotonic counter is
    // fine when the only way onto the canvas is asking for a terminal, and wrong
    // now that every window is adopted automatically: closing and reopening things
    // over a session would walk the counter up without bound and eventually place
    // new windows pages away from anything, with the freed slots left empty behind
    // them.
    //
    // Scanning up to size()+1 always terminates with a free slot: N managed windows
    // can occupy at most N distinct placements, so one of the first N+1 is unused.
    CanvasPlacement placement = slot_placement(0);
    for (std::size_t slot = 0; slot <= placements_.size(); ++slot) {
        const CanvasPlacement candidate = slot_placement(static_cast<int>(slot));
        const bool taken = std::any_of(
            placements_.begin(), placements_.end(),
            [&](const auto& entry) { return entry.second == candidate; });
        if (!taken) {
            placement = candidate;
            break;
        }
    }

    place(window_id, placement);
    return placement;
}

void Canvas::forget(const std::string& window_id) { placements_.erase(window_id); }

bool Canvas::manages(const std::string& window_id) const {
    return placements_.count(window_id) != 0;
}

std::vector<std::string> Canvas::managed_ids() const {
    // std::map, so this is sorted by id and therefore stable across passes --
    // which is what lets a caller compare two reconcile plans for equality.
    std::vector<std::string> ids;
    ids.reserve(placements_.size());
    for (const auto& [id, _] : placements_) ids.push_back(id);
    return ids;
}

std::optional<CanvasPlacement> Canvas::placement_of(const std::string& window_id) const {
    const auto it = placements_.find(window_id);
    if (it == placements_.end()) return std::nullopt;
    return it->second;
}

void Canvas::note_screen_move(const std::string& window_id, int screen_x, int screen_y) {
    const auto it = placements_.find(window_id);
    if (it == placements_.end()) return;

    // Screen space back to canvas space. Without this a user drag would be silently
    // reverted by the next pan, since resolve() derives screen from canvas.
    it->second.x = static_cast<int>(screen_x / zoom_) + viewport_.x;
    it->second.y = static_cast<int>(screen_y / zoom_) + viewport_.y;
}

void Canvas::set_hidden(std::set<std::string> hidden) { hidden_ = std::move(hidden); }

bool Canvas::is_hidden(const std::string& window_id) const {
    return hidden_.count(window_id) != 0;
}

bool Canvas::set_zoom(double zoom) {
    const double next = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (std::abs(next - zoom_) < 1e-9) return false;

    // The viewport ORIGIN is the anchor, not its centre.
    //
    // Centre-anchoring is the right choice for a canvas that can scroll freely in
    // every direction, and the wrong one for a canvas that owns a single monitor.
    // Holding the centre fixed means zooming IN slides the viewport origin forward
    // into canvas space, which puts every window that was at the left or top edge
    // at a NEGATIVE screen coordinate -- off the side of the monitor. There is no
    // free space there to hang off into, only the user's other screen, so those
    // windows are parked. Measured: from a normal two-column fit, one press of "+"
    // sent both windows off the canvas at once and the monitor went empty.
    //
    // Anchoring the origin means zoom only ever grows or shrinks the layout away
    // from the top-left corner the fit already anchored it to. Nothing acquires a
    // negative coordinate, so nothing is pushed off an edge it cannot hang over.
    zoom_ = next;
    return true;
}

bool Canvas::reset_zoom() { return set_zoom(1.0); }

bool Canvas::zoom_by(double factor) {
    if (factor <= 0.0) return false;
    return set_zoom(zoom_ * factor);
}

void Canvas::pan_by(int dx, int dy) {
    // The arguments are screen pixels -- that is what a drag or a scroll notch
    // produces -- so they are converted to canvas units before moving. Without the
    // division, a drag at 0.5x would move the content half as far as the hand and
    // feel like it was sticking.
    viewport_.x += static_cast<int>(dx / zoom_);
    viewport_.y += static_cast<int>(dy / zoom_);
}

bool Canvas::focus_on(const std::string& window_id, int window_width,
                      int window_height) {
    const auto placement = placement_of(window_id);
    if (!placement) return false;

    viewport_.x = placement->x - (viewport_.width - window_width) / 2;
    viewport_.y = placement->y - (viewport_.height - window_height) / 2;
    return true;
}

std::vector<ScreenPosition> Canvas::resolve(bool include_life_size) const {
    std::vector<ScreenPosition> result;
    result.reserve(placements_.size());

    for (const auto& [id, placement] : placements_) {
        // Not merely invisible -- absent. A hidden window must not even be parked,
        // because parking is a move and a move restores it.
        if (hidden_.count(id) != 0) continue;

        ScreenPosition position;
        position.id = id;
        // Scaled toward the viewport ORIGIN, so zooming out only ever shrinks the
        // area the windows occupy -- it cannot push anything past an edge.
        //
        // The first version kept the viewport CENTRE fixed, which is right for a
        // canvas that can scroll freely in every direction and wrong for one that
        // owns a single monitor: the compensating viewport shift walked windows
        // downward until their centres left the screen, at which point they were
        // released as "dragged to another monitor" and stopped responding to zoom
        // at all. Scaling from the origin has no compensating shift to get wrong.
        position.x  = static_cast<int>((placement.x - viewport_.x) * zoom_);
        position.y  = static_cast<int>((placement.y - viewport_.y) * zoom_);

        // A size is normally requested only when zoomed away from life size.
        // Explicit zoom operations pass include_life_size so the final step back
        // to 1.0 restores the real X11 window instead of merely changing our model
        // while leaving the window physically shrunken.
        if ((zoom_ != 1.0 || include_life_size) &&
            placement.width > 0 && placement.height > 0) {
            position.width  = std::max(1, static_cast<int>(placement.width  * zoom_));
            position.height = std::max(1, static_cast<int>(placement.height * zoom_));
        }

        // A window is shown only if it fits ENTIRELY within the viewport.
        //
        // The generous version of this test -- any overlap counts -- lets a window
        // straddle the edge, and on X11 a window that straddles the edge of one
        // monitor is physically drawn across the next one, because there is no way
        // to clip a window we do not own. Panning right by half a screen put half a
        // terminal on the display to the left, in full view. The canvas owns one
        // monitor, so anything that would spill is parked instead.
        //
        // The cost is that windows pop in and out at the edges of a pan rather than
        // sliding smoothly across them. That is the honest trade on X11: the only
        // alternative is letting the canvas leak onto screens it does not own.
        //
        // What this rule must NOT do is hide a window that has nowhere better to be.
        // See the oversized case below.
        const int width  = placement.width  > 0
                               ? static_cast<int>(placement.width  * zoom_) : 0;
        const int height = placement.height > 0
                               ? static_cast<int>(placement.height * zoom_) : 0;

        // The usable band excludes the panels. fit_all() already honoured the
        // struts, but panning did not -- so a window laid out clear of the bars
        // slid straight under them the moment the canvas moved, and the top and
        // bottom of the canvas were dead space you could put things into but never
        // see. The reserved edges are not part of the canvas at all.
        const int top    = inset_top_;
        const int bottom = viewport_.height - inset_bottom_;

        // A window bigger than the band can never satisfy "fits entirely", and
        // parking it would mean it silently vanished -- open something larger than
        // the screen, or drag a window taller than the gap between the bars, and the
        // desktop would appear to swallow it. Those are judged on their ORIGIN
        // instead: shown, anchored at the edge they start from, with apply_positions
        // capping the size so the overflow still cannot reach the next monitor.
        const int band      = std::max(1, bottom - top);
        const bool oversized = width > viewport_.width || height > band;

        position.visible =
            oversized
                ? (position.x >= 0 && position.x < viewport_.width &&
                   position.y >= top && position.y < bottom)
                : (position.x >= 0 && position.y >= top &&
                   position.x + width  <= viewport_.width &&
                   position.y + height <= bottom);

        result.push_back(std::move(position));
    }
    return result;
}

std::optional<Rect> Canvas::content_bounds(int tile_width, int tile_height) const {
    if (placements_.empty()) return std::nullopt;

    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool first = true;
    for (const auto& [id, placement] : placements_) {
        const int right  = placement.x + std::max(1, tile_width);
        const int bottom = placement.y + std::max(1, tile_height);
        if (first) {
            min_x = placement.x; min_y = placement.y;
            max_x = right;       max_y = bottom;
            first = false;
            continue;
        }
        min_x = std::min(min_x, placement.x);
        min_y = std::min(min_y, placement.y);
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);
    }

    return Rect{.x = min_x, .y = min_y, .width = max_x - min_x, .height = max_y - min_y};
}

// ---------------------------------------------------------------------------
Rect park_slot(const Rect& monitor, const Rect& screen) {
    return Rect{.x = monitor.x,
                .y = screen.y + screen.height,
                .width = monitor.width,
                .height = monitor.height};
}

bool apply_positions(const std::vector<ScreenPosition>& positions, const Rect& monitor,
                     const Rect* screen) {
    bool all_ok = true;

    for (const auto& position : positions) {
        int x = position.x + monitor.x;
        int y = position.y + monitor.y;

        // Parked, not merely off-viewport. resolve() has already decided this window
        // does not fit; without the redirect it is moved to wherever its canvas
        // coordinate happens to land, and on a multi-head desk that is the monitor
        // next door rather than nowhere.
        //
        // Only when a screen was supplied. With one monitor the union IS the
        // monitor, and following the canvas coordinate off the edge is both correct
        // and cheaper than a relocation.
        bool parking = screen != nullptr && !position.visible;

        // A maximised window ignores position AND size changes entirely, so the
        // state must be cleared first. The result is deliberately not checked: a
        // window that was never maximised reports failure, which is fine.
        //
        // The SLEEP is the fix for a real bug, not padding. Unmaximising is
        // asynchronous -- the request goes to the window manager and returns
        // immediately -- so a move issued in the next statement was being processed
        // while the window was still maximised, and silently discarded. Traced: the
        // canvas asked for y=-1 and the window sat at y=48 across five identical
        // attempts, with _NET_WM_STATE still reporting MAXIMIZED_VERT. After a
        // manual unmaximise the very same call landed exactly where asked.
        //
        // Only when a resize is requested, which means a fit or a zoom. Those are
        // user-initiated and infrequent; a pan must never pay this cost, and a pan
        // cannot hit the bug because a maximised window is not being resized.
        // What the window is allowed to occupy, once it is on this monitor.
        //
        // A zoom scales sizes without bound, and a window grown past the right or
        // bottom edge does not stop at the bezel -- it carries on across the user's
        // next screen, because X11 has no way to clip a window we do not own. Capping
        // here is what keeps the canvas on one monitor: zoom in and windows grow
        // until they reach the edge, then stop.
        //
        // Only when a screen union was supplied. With a single monitor there is
        // nothing to spill onto and the cap would just be a smaller zoom.
        int want_width  = position.width;
        int want_height = position.height;
        if (screen != nullptr && !parking && want_width > 0 && want_height > 0) {
            const int capped_width  = std::min(want_width,  monitor.width  - position.x);
            const int capped_height = std::min(want_height, monitor.height - position.y);

            // A cap only works if the window agrees to it. Applications set minimum
            // sizes -- xfce4-terminal will not go below about 285px, because that is
            // its narrowest whole number of character cells -- and a window that
            // refuses to shrink stays its old width and hangs over the edge anyway.
            // Measured: asked for 45px, kept 285, and 240px of it sat on the display
            // next door.
            //
            // So a squeeze that would go below what applications generally honour is
            // not attempted at all. The window is parked instead, which is the one
            // outcome that is certain not to spill.
            if (capped_width < kMinPlacedWidth || capped_height < kMinPlacedHeight) {
                parking = true;
            } else {
                want_width  = capped_width;
                want_height = capped_height;
            }
        }

        if (parking) {
            const Rect slot = park_slot(monitor, *screen);
            x = slot.x;
            y = slot.y;
        }

        // A parked window is not resized. Nobody can see it, and each resize costs
        // two 60ms settling sleeps -- on a zoom that parks half the canvas that is
        // the difference between a control that responds and one that stutters.
        const bool resizing = !parking && want_width > 0 && want_height > 0;
        if (resizing) {
            display().unmaximize_window(position.id);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        // move_window's contract is outer-frame coordinates. Do not subtract the
        // decorations here: the X11 backend now uses wmctrl's frame-aware EWMH
        // move. Subtracting them made the title bar start behind the top panel and
        // left an equally misleading strip of wallpaper above the bottom panel.
        const auto frame = display().frame_extents(position.id);

        // RESIZE BEFORE MOVE. This order is load bearing.
        //
        // Traced: asking move(1915,-1) then size(1920,1020) put the client at y=86,
        // a further 29px -- one title bar -- below where the move alone would have
        // left it. The window manager re-places a window when it is resized, so the
        // move was being applied and then partly undone. Resizing first and moving
        // second makes the move the last word, which is what the layout needs.
        //
        // Only a fit pass sets a size. Resizing on every pan would be both
        // gratuitous and destructive -- it would silently overwrite whatever size
        // the user had chosen for every window, every time they scrolled.
        if (resizing) {
            // The requested size describes the space the window should OCCUPY, but
            // resize_window sets the CLIENT size -- and the window manager then
            // draws a title bar and borders around that. Asking for exactly the gap
            // between the panels therefore produced a window one title bar taller
            // than the gap, overhanging the bottom bar. Every window sat lower than
            // it should and the bottom edge of the canvas was unusable.
            //
            // Subtracting the frame is the whole fix. Extents are read per window
            // because they differ -- an undecorated window reports zeros and is
            // unaffected -- and only when a resize was actually requested, which is
            // a fit or a zoom, never a pan.
            int width  = want_width;
            int height = want_height;
            if (frame) {
                width  = std::max(1, width  - frame->left - frame->right);
                height = std::max(1, height - frame->top  - frame->bottom);
            }
            all_ok = display().resize_window(position.id, width, height) && all_ok;

            // Terminals quantise their client size to whole character cells and
            // apply that configure asynchronously. If the frame move follows
            // immediately, the later cell-size adjustment pulls it a few pixels
            // back under the top panel. Let resizing settle so the frame-aware
            // move below is truly the final geometry operation.
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        // AUSPEX_TRACE_PLACEMENT=1 prints what this function decided, so a window
        // landing somewhere other than where the layout intended can be diagnosed
        // by reading rather than by inference. Kept because every placement bug so
        // far has been a disagreement between two correct-looking pieces of code,
        // and that is exactly what a trace makes obvious.
        if (std::getenv("AUSPEX_TRACE_PLACEMENT") != nullptr) {
            std::fprintf(stderr,
                         "place %s vis=%d in(%d,%d %dx%d) frame(l%d t%d r%d b%d) "
                         "-> move(%d,%d) size(%d,%d)\n",
                         position.id.c_str(), position.visible ? 1 : 0,
                         position.x, position.y, want_width, want_height,
                         frame ? frame->left : -1, frame ? frame->top : -1,
                         frame ? frame->right : -1, frame ? frame->bottom : -1,
                         x, y, want_width, want_height);
        }

        all_ok = display().move_window(position.id, x, y) && all_ok;

    }
    return all_ok;
}

// ---------------------------------------------------------------------------
FitLayout compute_fit_layout(int count, int width, int height) {
    if (count <= 0 || width <= 0 || height <= 0) return {};

    // The objective is tile SHAPE, not tile area.
    //
    // Area is very nearly constant across grids -- four windows in one column and
    // four in a 2x2 both use the whole screen -- so maximising it just picks
    // whichever grid rounds up best, which is noise. What actually differs is the
    // shape of the cell: one column gives four letterbox slots nothing renders well
    // in, and 2x2 gives four small screens.
    //
    // So: pick the grid whose tile aspect ratio is closest to the screen's. The
    // comparison is in log space because aspect error is multiplicative -- a tile
    // twice as wide as it should be is exactly as wrong as one half as wide, and
    // plain subtraction says otherwise.
    const double target = static_cast<double>(width) / static_cast<double>(height);

    FitLayout best{};
    double best_score = std::numeric_limits<double>::max();

    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;   // ceil(count/columns)

        // Below this a window is present but useless. Stop shrinking and let the
        // grid overflow the viewport instead of producing unreadable slivers.
        const int tile_w = std::max(width / columns, kMinTileWidth);
        const int tile_h = std::max(height / rows, kMinTileHeight);

        const double aspect = static_cast<double>(tile_w) / static_cast<double>(tile_h);
        const double score  = std::abs(std::log(aspect / target));

        // Ties go to more columns. Two windows on a wide screen are equally wrong
        // stacked or side by side by this measure, and side by side is what people
        // mean.
        constexpr double kEpsilon = 1e-9;
        if (score < best_score - kEpsilon ||
            (score < best_score + kEpsilon && columns > best.columns)) {
            best_score = std::min(best_score, score);
            best = {.columns = columns, .rows = rows,
                    .tile_width = tile_w, .tile_height = tile_h};
        }
    }
    return best;
}

FitLayout Canvas::fit_all() {
    // Only the windows you can see get a share of the screen. Counting minimised
    // ones would leave a gap in the grid for every window that is not there.
    std::vector<std::string> showing;
    for (const auto& id : managed_ids()) {
        if (hidden_.count(id) == 0) showing.push_back(id);
    }

    const int count = static_cast<int>(showing.size());
    if (count == 0) return {};

    const int usable_height =
        std::max(1, viewport_.height - inset_top_ - inset_bottom_);
    const FitLayout layout = compute_fit_layout(count, viewport_.width, usable_height);

    // Row-major over managed_ids(), which is sorted, so the arrangement is stable:
    // fitting twice with the same windows puts them in the same cells rather than
    // shuffling them under the user's hand.
    int index = 0;
    for (const auto& id : showing) {
        const int column = index % layout.columns;
        const int row    = index / layout.columns;
        // Assign the POSITION only. Overwriting the whole placement here wiped the
        // natural width/height recorded at adoption, and zoom has nothing to scale
        // from without it -- the symptom was windows that moved when you zoomed but
        // never changed size.
        placements_[id].x = viewport_.x + column * layout.tile_width;
        placements_[id].y = viewport_.y + inset_top_ + row * layout.tile_height;

        // A fit also redefines how big these windows are, so the tile becomes the
        // new baseline. Otherwise zooming after a fit would scale from whatever
        // size they were before the fit and jump.
        // Canvas units, so the tile is divided by the zoom for the same reason the
        // adoption path divides the observed geometry: everything stored in a
        // placement is at zoom 1.0, and only resolve() scales it.
        placements_[id].width  = static_cast<int>(layout.tile_width  / zoom_);
        placements_[id].height = static_cast<int>(layout.tile_height / zoom_);
        ++index;
    }
    return layout;
}

// ---------------------------------------------------------------------------
CanvasSync plan_canvas_sync(const std::vector<WindowEntry>& live, const Canvas& canvas) {
    CanvasSync sync;

    std::set<std::string> live_ids;
    for (const auto& window : live) {
        if (window.id.empty()) continue;   // wmctrl gave us a line we cannot act on
        live_ids.insert(window.id);
        if (!canvas.manages(window.id)) sync.adopt.push_back(window.id);
    }

    for (const auto& id : canvas.managed_ids()) {
        if (live_ids.count(id) == 0) sync.forget.push_back(id);
    }

    return sync;
}

void apply_canvas_sync(Canvas& canvas, const CanvasSync& sync, int tile_width,
                       int tile_height) {
    // Forget first. Dropping closed windows before placing new ones means a freed
    // tile slot can be reused in the same pass, so closing and reopening a terminal
    // does not march the canvas endlessly to the right.
    for (const auto& id : sync.forget) canvas.forget(id);
    for (const auto& id : sync.adopt) canvas.place_next(id, tile_width, tile_height);
}

}  // namespace auspex
