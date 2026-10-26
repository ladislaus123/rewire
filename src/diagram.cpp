#include "diagram.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <tuple>

namespace rewire {
namespace {

struct Box {
    std::string id;
    std::string title;
    RelationKind link = RelationKind::Association;
    bool focused = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    std::vector<std::string> lines;
};

class Grid {
public:
    void ensure(int x, int y) {
        if (x < 0 || y < 0 || x > 220 || y > 80) {
            return;
        }
        if (y >= static_cast<int>(rows_.size())) {
            rows_.resize(static_cast<std::size_t>(y) + 1);
        }
        if (x >= static_cast<int>(rows_[static_cast<std::size_t>(y)].size())) {
            rows_[static_cast<std::size_t>(y)].resize(static_cast<std::size_t>(x) + 1, ' ');
        }
        width_ = std::max(width_, x + 1);
        height_ = std::max(height_, y + 1);
    }

    char at(int x, int y) const {
        if (y < 0 || x < 0 || y >= static_cast<int>(rows_.size())) {
            return ' ';
        }
        const auto& row = rows_[static_cast<std::size_t>(y)];
        if (x >= static_cast<int>(row.size())) {
            return ' ';
        }
        return row[static_cast<std::size_t>(x)];
    }

    void force(int x, int y, char ch) {
        ensure(x, y);
        if (y < 0 || x < 0 || y >= static_cast<int>(rows_.size())) {
            return;
        }
        auto& row = rows_[static_cast<std::size_t>(y)];
        if (x >= static_cast<int>(row.size())) {
            return;
        }
        row[static_cast<std::size_t>(x)] = ch;
    }

    void plot(int x, int y, char ch) {
        char cur = at(x, y);
        if (cur == ' ') {
            force(x, y, ch);
            return;
        }
        if (cur == '+' || (cur == '-' && ch == '|') || (cur == '|' && ch == '-')) {
            force(x, y, '+');
        }
    }

    void text(int x, int y, const std::string& value, bool overwrite) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            ensure(x + static_cast<int>(i), y);
            if (y < 0 || y >= static_cast<int>(rows_.size())) {
                return;
            }
            auto& row = rows_[static_cast<std::size_t>(y)];
            int column = x + static_cast<int>(i);
            if (column < 0 || column >= static_cast<int>(row.size())) {
                continue;
            }
            if (overwrite || row[static_cast<std::size_t>(column)] == ' ') {
                row[static_cast<std::size_t>(column)] = value[i];
            }
        }
    }

    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        out.reserve(rows_.size());
        for (const auto& row : rows_) {
            std::string line(row.begin(), row.end());
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            out.push_back(std::move(line));
        }
        return out;
    }

private:
    std::vector<std::string> rows_;
    int width_ = 0;
    int height_ = 0;
};

std::string clip(std::string value, std::size_t width) {
    if (value.size() <= width) {
        return value;
    }
    if (width <= 1) {
        return value.substr(0, width);
    }
    value.resize(width - 1);
    value.push_back('~');
    return value;
}

Box make_box(const Class& item, bool focused) {
    Box box;
    box.id = item.qualified_name;
    box.title = item.name.empty() ? short_name(item.qualified_name) : item.name;
    box.focused = focused;
    std::vector<std::string> body;
    body.push_back(box.title);
    int shown = 0;
    for (const auto& field : item.fields) {
        if (shown++ >= 5) {
            break;
        }
        std::string line = (field.access == "private" ? "-" : field.access == "protected" ? "#" : "+");
        line += " " + field.name;
        if (!field.target.empty()) {
            line += " " + std::string(relation_marker(field.relation));
        }
        line += " : " + field.type_spelling;
        body.push_back(line);
    }
    if (static_cast<int>(item.fields.size()) > 5) {
        body.emplace_back("…");
    }
    shown = 0;
    for (const auto& method : item.methods) {
        if (shown++ >= 4) {
            break;
        }
        std::string line = (method.access == "private" ? "-" : method.access == "protected" ? "#" : "+");
        line += " " + method.signature();
        body.push_back(line);
    }
    if (static_cast<int>(item.methods.size()) > 4) {
        body.emplace_back("…");
    }
    std::size_t inner = 8;
    for (const auto& line : body) {
        inner = std::max(inner, line.size());
    }
    inner = std::min<std::size_t>(inner, 42);
    box.w = static_cast<int>(inner) + 2;
    box.h = static_cast<int>(body.size()) + 3;
    box.lines.push_back(clip(body.front(), inner));
    for (std::size_t i = 1; i < body.size(); ++i) {
        box.lines.push_back(clip(body[i], inner));
    }
    return box;
}

void draw_box(Grid& grid, const Box& box) {
    grid.text(box.x, box.y, "+" + std::string(static_cast<std::size_t>(box.w - 2), '-') + "+", true);
    grid.text(box.x, box.y + 1, "|" + clip(box.lines.front(), static_cast<std::size_t>(box.w - 2)) + "|", true);
    std::string pad(static_cast<std::size_t>(box.w - 2), ' ');
    auto title = clip(box.lines.empty() ? box.title : box.lines.front(), static_cast<std::size_t>(box.w - 2));
    pad.replace(0, title.size(), title);
    grid.text(box.x, box.y + 1, "|" + pad + "|", true);
    grid.text(box.x, box.y + 2, "+" + std::string(static_cast<std::size_t>(box.w - 2), '-') + "+", true);
    for (std::size_t i = 1; i < box.lines.size(); ++i) {
        std::string row(static_cast<std::size_t>(box.w - 2), ' ');
        auto text = clip(box.lines[i], static_cast<std::size_t>(box.w - 2));
        row.replace(0, text.size(), text);
        grid.text(box.x, box.y + 2 + static_cast<int>(i), "|" + row + "|", true);
    }
    grid.text(box.x, box.y + box.h - 1, "+" + std::string(static_cast<std::size_t>(box.w - 2), '-') + "+", true);
}

struct Mark {
    int x = 0;
    int y = 0;
    char ch = 'v';
};

bool in_box(const std::vector<Box>& boxes, int x, int y) {
    for (const auto& box : boxes) {
        if (x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h) {
            return true;
        }
    }
    return false;
}

// Route around other classes. The line stays in the gaps; the arrow is stamped
// onto the target border after the boxes are drawn.
void connect(Grid& grid, const std::vector<Box>& boxes, const Box& from, const Box& to, RelationKind kind,
             std::vector<Mark>& marks) {
    const int limit_x = 200;
    const int limit_y = 60;
    int x1 = from.x + from.w / 2;
    int y1 = from.y + from.h;
    int x2 = to.x + to.w / 2;
    int y2 = to.y - 1;
    int adx = 0;
    int ady = 1;
    if (to.y >= from.y + from.h) {
        x1 = from.x + from.w / 2;
        y1 = from.y + from.h;
        x2 = to.x + to.w / 2;
        y2 = to.y - 1;
        adx = 0;
        ady = 1;
    } else if (from.y >= to.y + to.h) {
        x1 = from.x + from.w / 2;
        y1 = from.y - 1;
        x2 = to.x + to.w / 2;
        y2 = to.y + to.h;
        adx = 0;
        ady = -1;
    } else if (to.x >= from.x + from.w) {
        x1 = from.x + from.w;
        y1 = from.y + from.h / 2;
        x2 = to.x - 1;
        y2 = to.y + to.h / 2;
        adx = 1;
        ady = 0;
    } else {
        x1 = from.x - 1;
        y1 = from.y + from.h / 2;
        x2 = to.x + to.w;
        y2 = to.y + to.h / 2;
        adx = -1;
        ady = 0;
    }
    if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 || x1 >= limit_x || y1 >= limit_y || x2 >= limit_x || y2 >= limit_y) {
        return;
    }

    const int dxs[4] = {0, 1, 0, -1};
    const int dys[4] = {-1, 0, 1, 0};
    auto id_of = [&](int x, int y, int dir) { return (y * limit_x + x) * 4 + dir; };
    std::vector<int> best(static_cast<std::size_t>(limit_x * limit_y * 4), 1000000000);
    std::vector<int> parent(best.size(), -1);
    using Item = std::tuple<int, int, int, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    auto blocked = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= limit_x || y >= limit_y) {
            return true;
        }
        if (x == x2 && y == y2) {
            return false;
        }
        return in_box(boxes, x, y);
    };
    if (in_box(boxes, x1, y1)) {
        return;
    }
    for (int dir = 0; dir < 4; ++dir) {
        int nx = x1 + dxs[dir];
        int ny = y1 + dys[dir];
        if (blocked(nx, ny)) {
            continue;
        }
        int id = id_of(nx, ny, dir);
        best[static_cast<std::size_t>(id)] = 8;
        parent[static_cast<std::size_t>(id)] = -2;
        queue.emplace(8, nx, ny, dir);
    }

    int found = -1;
    while (!queue.empty()) {
        auto [cost, x, y, dir] = queue.top();
        queue.pop();
        int id = id_of(x, y, dir);
        if (cost != best[static_cast<std::size_t>(id)]) {
            continue;
        }
        if (x == x2 && y == y2) {
            found = id;
            break;
        }
        for (int next = 0; next < 4; ++next) {
            int nx = x + dxs[next];
            int ny = y + dys[next];
            if (blocked(nx, ny)) {
                continue;
            }
            int bend = next == dir ? 0 : 1;
            int next_cost = cost + 8 + bend;
            int next_id = id_of(nx, ny, next);
            if (next_cost >= best[static_cast<std::size_t>(next_id)]) {
                continue;
            }
            best[static_cast<std::size_t>(next_id)] = next_cost;
            parent[static_cast<std::size_t>(next_id)] = id;
            queue.emplace(next_cost, nx, ny, next);
        }
    }

    std::vector<std::pair<int, int>> path;
    if (found >= 0) {
        for (int cursor = found; cursor >= 0;) {
            int linear = cursor / 4;
            path.emplace_back(linear % limit_x, linear / limit_x);
            int previous = parent[static_cast<std::size_t>(cursor)];
            if (previous == -2) {
                break;
            }
            cursor = previous;
        }
        path.emplace_back(x1, y1);
        std::reverse(path.begin(), path.end());
    } else {
        path.emplace_back(x1, y1);
        int x = x1;
        int y = y1;
        while (y != y2) {
            y += y2 > y ? 1 : -1;
            if (!in_box(boxes, x, y) || (x == x2 && y == y2)) {
                path.emplace_back(x, y);
            }
        }
        while (x != x2) {
            x += x2 > x ? 1 : -1;
            if (!in_box(boxes, x, y) || (x == x2 && y == y2)) {
                path.emplace_back(x, y);
            }
        }
    }

    for (std::size_t i = 0; i < path.size(); ++i) {
        char ch = '|';
        if (i == 0) {
            ch = path.size() > 1 && path[1].first != path[0].first ? '-' : '|';
        } else if (i + 1 == path.size()) {
            ch = path[i].first != path[i - 1].first ? '-' : '|';
        } else {
            bool from_horizontal = path[i].first != path[i - 1].first;
            bool to_horizontal = path[i + 1].first != path[i].first;
            if (from_horizontal != to_horizontal) {
                ch = '+';
            } else {
                ch = from_horizontal ? '-' : '|';
            }
        }
        grid.plot(path[i].first, path[i].second, ch);
    }
    if (kind != RelationKind::Inheritance && !path.empty()) {
        grid.force(path.front().first, path.front().second, relation_marker(kind)[0]);
    }
    int ax = x2 + adx;
    int ay = y2 + ady;
    if (ax < 0 || ay < 0 || ax >= limit_x || ay >= limit_y) {
        return;
    }
    char arrow = ady > 0 ? 'v' : ady < 0 ? '^' : adx > 0 ? '>' : '<';
    if (kind == RelationKind::Inheritance) {
        arrow = ady < 0 ? '^' : arrow;
    }
    marks.push_back(Mark{ax, ay, arrow});
}

int place_row(std::vector<Box>& boxes, int y, int left) {
    int x = left;
    int height = 0;
    for (auto& box : boxes) {
        box.x = x;
        box.y = y;
        x += box.w + 3;
        height = std::max(height, box.h);
    }
    return height;
}

const Class* find_exact(const ProjectModel& model, const std::string& name) {
    for (const auto& item : model.classes) {
        if (item.qualified_name == name) {
            return &item;
        }
    }
    for (const auto& item : model.classes) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<std::string> render_class_diagram(const ProjectModel& model, const std::string& focus, bool show_all,
                                               int max_columns) {
    if (model.classes.empty()) {
        return {"No classes found under " + model.root + ".",
                "Open a project with C, C++, Java, Python, TypeScript, or Rust sources."};
    }
    const Class* selected = find_exact(model, focus);
    if (!selected) {
        selected = &model.classes.front();
    }
    std::vector<std::string> footer;
    footer.push_back("");
    footer.push_back("Relationships  * composition   o aggregation   + association   . dependency   ^ inheritance");
    auto add_rel = [&](const Relation& relation) {
        footer.push_back("  " + short_name(relation.from) + " --" + relation_name(relation.kind) + "--> " +
                         short_name(relation.to) + (relation.label.empty() ? "" : " (" + relation.label + ")"));
    };
    if (show_all) {
        Grid grid;
        std::vector<Box> boxes;
        for (const auto& item : model.classes) {
            boxes.push_back(make_box(item, item.qualified_name == selected->qualified_name));
            if (boxes.size() >= 24) {
                break;
            }
        }
        int y = 0;
        std::size_t index = 0;
        while (index < boxes.size()) {
            std::vector<Box> row;
            int used = 0;
            while (index < boxes.size()) {
                int width = boxes[index].w + 3;
                if (!row.empty() && used + width > std::max(40, max_columns)) {
                    break;
                }
                row.push_back(boxes[index]);
                used += width;
                ++index;
            }
            place_row(row, y, 0);
            int height = 0;
            for (const auto& box : row) {
                draw_box(grid, box);
                height = std::max(height, box.h);
            }
            y += height + 1;
        }
        auto lines = grid.lines();
        lines.push_back("");
        lines.push_back("All classes. Focus one class for relationship lines.");
        int shown = 0;
        for (const auto& relation : model.relations) {
            if (shown++ >= 40) {
                lines.push_back("  …");
                break;
            }
            add_rel(relation);
        }
        lines.insert(lines.end(), footer.begin(), footer.end());
        return lines;
    }

    std::map<std::string, RelationKind> linked;
    std::vector<std::string> bases, parts, dependencies, owners, derived;
    auto take = [&](std::vector<std::string>& bucket, const std::string& name, RelationKind kind) {
        if (name.empty() || name == selected->qualified_name) {
            return;
        }
        if (std::find(bucket.begin(), bucket.end(), name) != bucket.end()) {
            return;
        }
        if (bucket.size() >= 4) {
            return;
        }
        bucket.push_back(name);
        linked[name] = kind;
    };
    for (const auto& relation : model.relations) {
        if (relation.from == selected->qualified_name) {
            if (relation.kind == RelationKind::Inheritance) {
                take(bases, relation.to, relation.kind);
            } else if (relation.kind == RelationKind::Dependency) {
                take(dependencies, relation.to, relation.kind);
            } else {
                take(parts, relation.to, relation.kind);
            }
            add_rel(relation);
        } else if (relation.to == selected->qualified_name) {
            if (relation.kind == RelationKind::Inheritance) {
                take(derived, relation.from, relation.kind);
            } else {
                take(owners, relation.from, relation.kind);
            }
            add_rel(relation);
        }
    }

    Box focus_box = make_box(*selected, true);
    std::vector<Box> base_boxes, part_boxes, dep_boxes, owner_boxes, derived_boxes;
    auto make_named = [&](const std::string& name) {
        const Class* item = find_exact(model, name);
        if (!item) {
            Class stub;
            stub.qualified_name = name;
            stub.name = short_name(name);
            return make_box(stub, false);
        }
        return make_box(*item, false);
    };
    for (const auto& name : bases) base_boxes.push_back(make_named(name));
    for (const auto& name : derived) derived_boxes.push_back(make_named(name));
    for (const auto& name : parts) part_boxes.push_back(make_named(name));
    for (const auto& name : dependencies) dep_boxes.push_back(make_named(name));
    for (const auto& name : owners) owner_boxes.push_back(make_named(name));

    int y = 0;
    if (!base_boxes.empty()) {
        y += place_row(base_boxes, y, 4) + 3;
    }
    focus_box.x = 8;
    focus_box.y = y;
    int mid_h = focus_box.h;
    if (!derived_boxes.empty()) {
        int x = focus_box.x + focus_box.w + 6;
        for (auto& box : derived_boxes) {
            box.x = x;
            box.y = y;
            x += box.w + 3;
            mid_h = std::max(mid_h, box.h);
        }
    }
    y += mid_h + 3;
    if (!part_boxes.empty()) {
        y += place_row(part_boxes, y, 2) + 3;
    }
    if (!dep_boxes.empty()) {
        y += place_row(dep_boxes, y, 2) + 3;
    }
    if (!owner_boxes.empty()) {
        place_row(owner_boxes, y, 2);
    }

    std::vector<Box> obstacles;
    obstacles.push_back(focus_box);
    obstacles.insert(obstacles.end(), base_boxes.begin(), base_boxes.end());
    obstacles.insert(obstacles.end(), derived_boxes.begin(), derived_boxes.end());
    obstacles.insert(obstacles.end(), part_boxes.begin(), part_boxes.end());
    obstacles.insert(obstacles.end(), dep_boxes.begin(), dep_boxes.end());
    obstacles.insert(obstacles.end(), owner_boxes.begin(), owner_boxes.end());

    Grid grid;
    std::vector<Mark> marks;
    for (const auto& box : base_boxes) connect(grid, obstacles, focus_box, box, RelationKind::Inheritance, marks);
    for (const auto& box : derived_boxes) connect(grid, obstacles, box, focus_box, RelationKind::Inheritance, marks);
    for (const auto& box : part_boxes) {
        connect(grid, obstacles, focus_box, box, linked.count(box.id) ? linked[box.id] : RelationKind::Association,
                marks);
    }
    for (const auto& box : dep_boxes) connect(grid, obstacles, focus_box, box, RelationKind::Dependency, marks);
    for (const auto& box : owner_boxes) {
        connect(grid, obstacles, box, focus_box, linked.count(box.id) ? linked[box.id] : RelationKind::Association,
                marks);
    }
    draw_box(grid, focus_box);
    for (const auto& box : base_boxes) draw_box(grid, box);
    for (const auto& box : derived_boxes) draw_box(grid, box);
    for (const auto& box : part_boxes) draw_box(grid, box);
    for (const auto& box : dep_boxes) draw_box(grid, box);
    for (const auto& box : owner_boxes) draw_box(grid, box);
    for (const auto& mark : marks) {
        grid.force(mark.x, mark.y, mark.ch);
    }
    auto lines = grid.lines();
    if (lines.empty()) {
        lines.push_back(selected->qualified_name);
    }
    lines.insert(lines.end(), footer.begin(), footer.end());
    return lines;
}

}  // namespace rewire
