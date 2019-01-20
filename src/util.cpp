#include "util.hpp"
#include <debug.hpp>

extern "C"
{
#include <wlr/util/region.h>
}

/* Geometry helpers */
bool operator == (const wf_geometry& a, const wf_geometry& b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool operator != (const wf_geometry& a, const wf_geometry& b)
{
    return !(a == b);
}

wf_point operator + (const wf_point& a, const wf_point& b)
{
    return {a.x + b.x, a.y + b.y};
}

wf_point operator + (const wf_point& a, const wf_geometry& b)
{
    return {a.x + b.x, a.y + b.y};
}

wf_geometry operator + (const wf_geometry &a, const wf_point& b)
{
    return {
        a.x + b.x,
        a.y + b.y,
        a.width,
        a.height
    };
}

wf_point operator - (const wf_point& a)
{
    return {-a.x, -a.y};
}

bool operator & (const wf_geometry& rect, const wf_point& point)
{
    return wlr_box_contains_point(&rect, point.x, point.y);
}

bool operator & (const wf_geometry& r1, const wf_geometry& r2)
{
    wlr_box result;
    return wlr_box_intersection(&result, &r1, &r2);
}

wf_geometry wf_geometry_intersection(const wf_geometry& r1,
    const wf_geometry& r2)
{
    wlr_box result;
    if (wlr_box_intersection(&result, &r1, &r2))
        return result;

    return {0, 0, 0, 0};
}

/* Pixman helpers */
wlr_box wlr_box_from_pixman_box(const pixman_box32_t& box)
{
    return {
        box.x1, box.y1,
        box.x2 - box.x1,
        box.y2 - box.y1
    };
}

pixman_box32_t pixman_box_from_wlr_box(const wlr_box& box)
{
    return {
        box.x, box.y,
        box.x + box.width,
        box.y + box.height
    };
}

wf_region::wf_region()
{
    pixman_region32_init(&_region);
}

wf_region::wf_region(pixman_region32_t *region) : wf_region()
{
    pixman_region32_copy(this->to_pixman(), region);
}

wf_region::wf_region(const wlr_box& box)
{
    pixman_region32_init_rect(&_region, box.x, box.y, box.width, box.height);
}

wf_region:: ~wf_region()
{
    pixman_region32_fini(&_region);
}

wf_region::wf_region(const wf_region& other) : wf_region()
{
    pixman_region32_copy(this->to_pixman(), other.unconst());
}

wf_region::wf_region(wf_region&& other) : wf_region()
{
    std::swap(this->_region, other._region);
}

wf_region& wf_region::operator = (const wf_region& other)
{
    if (&other == this)
        return *this;

    pixman_region32_copy(&_region, other.unconst());
    return *this;
}

wf_region& wf_region::operator = (wf_region&& other)
{
    if (&other == this)
        return *this;

    std::swap(_region, other._region);
    return *this;
}

bool wf_region::empty() const
{
    return !pixman_region32_not_empty(this->unconst());
}

void wf_region::clear()
{
    pixman_region32_clear(&_region);
}

void wf_region::expand_edges(int amount)
{
    /* FIXME: make sure we don't throw pixman errors when amount is bigger
     * than a rectangle size */
    wlr_region_expand(this->to_pixman(), this->to_pixman(), amount);
}

pixman_box32_t wf_region::get_extents() const
{
    return *pixman_region32_extents(this->unconst());
}

/* Translate the region */
wf_region wf_region::operator + (const wf_point& vector) const
{
    wf_region result{*this};
    pixman_region32_translate(&result._region, vector.x, vector.y);
    return result;
}

wf_region& wf_region::operator += (const wf_point& vector)
{
    pixman_region32_translate(&_region, vector.x, vector.y);
    return *this;
}

wf_region wf_region::operator * (float scale) const
{
    wf_region result;
    wlr_region_scale(result.to_pixman(), this->unconst(), scale);
    return result;
}

wf_region& wf_region::operator *= (float scale)
{
    wlr_region_scale(this->to_pixman(), this->to_pixman(), scale);
    return *this;
}

/* Region intersection */
wf_region wf_region::operator & (const wlr_box& box) const
{
    wf_region result;
    pixman_region32_intersect_rect(result.to_pixman(), this->unconst(),
        box.x, box.y, box.width, box.height);

    return result;
}

wf_region wf_region::operator & (const wf_region& other) const
{
    wf_region result;
    pixman_region32_intersect(result.to_pixman(),
        this->unconst(), other.unconst());

    return result;
}

wf_region& wf_region::operator &= (const wlr_box& box)
{
    pixman_region32_intersect_rect(this->to_pixman(), this->to_pixman(),
        box.x, box.y, box.width, box.height);
    return *this;
}

wf_region& wf_region::operator &= (const wf_region& other)
{
    pixman_region32_intersect(this->to_pixman(),
        this->to_pixman(), other.unconst());
    return *this;
}

/* Region union */
wf_region wf_region::operator | (const wlr_box& other) const
{
    wf_region result;
    pixman_region32_union_rect(result.to_pixman(), this->unconst(),
        other.x, other.y, other.width, other.height);
    return result;
}

wf_region wf_region::operator | (const wf_region& other) const
{
    wf_region result;
    pixman_region32_union(result.to_pixman(), this->unconst(), other.unconst());
    return result;
}

wf_region& wf_region::operator |= (const wlr_box& other)
{
    pixman_region32_union_rect(this->to_pixman(), this->to_pixman(),
        other.x, other.y, other.width, other.height);
    return *this;
}

wf_region& wf_region::operator |= (const wf_region& other)
{
    pixman_region32_union(this->to_pixman(), this->to_pixman(), other.unconst());
    return *this;
}

/* Subtract the box/region from the current region */
wf_region wf_region::operator ^ (const wlr_box& box) const
{
    wf_region result;
    wf_region sub{box};
    pixman_region32_subtract(result.to_pixman(), this->unconst(), sub.to_pixman());
    return result;
}

wf_region wf_region::operator ^ (const wf_region& other) const
{
    wf_region result;
    pixman_region32_subtract(result.to_pixman(),
        this->unconst(), other.unconst());
    return result;
}

wf_region& wf_region::operator ^= (const wlr_box& box)
{
    wf_region sub{box};
    pixman_region32_subtract(this->to_pixman(),
        this->to_pixman(), sub.to_pixman());
    return *this;
}

wf_region& wf_region::operator ^= (const wf_region& other)
{
    pixman_region32_subtract(this->to_pixman(),
        this->to_pixman(), other.unconst());
    return *this;
}

pixman_region32_t *wf_region::to_pixman()
{
    return &_region;
}

pixman_region32_t* wf_region::unconst() const
{
    return const_cast<pixman_region32_t*> (&_region);
}

const pixman_box32_t* wf_region::begin() const
{
    int n;
    return pixman_region32_rectangles(unconst(), &n);
}

const pixman_box32_t* wf_region::end() const
{
    int n;
    auto data = pixman_region32_rectangles(unconst(), &n);
    return data + n;
}

/* Misc helper functions */
int64_t timespec_to_msec(const timespec& ts)
{
    return ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

uint32_t get_current_time()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return timespec_to_msec(ts);
}
