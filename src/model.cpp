#include "model.hpp"

#include <algorithm>
#include <sstream>

namespace rewire {

const char* relation_name(RelationKind kind) {
    switch (kind) {
        case RelationKind::Inheritance: return "inheritance";
        case RelationKind::Composition: return "composition";
        case RelationKind::Aggregation: return "aggregation";
        case RelationKind::Association: return "association";
        case RelationKind::Dependency: return "dependency";
    }
    return "dependency";
}

const char* relation_marker(RelationKind kind) {
    switch (kind) {
        case RelationKind::Inheritance: return "^";
        case RelationKind::Composition: return "*";
        case RelationKind::Aggregation: return "o";
        case RelationKind::Association: return "+";
        case RelationKind::Dependency: return ".";
    }
    return ".";
}

std::string short_name(const std::string& qualified) {
    const auto cols = qualified.rfind("::");
    const auto dots = qualified.rfind('.');
    if (cols == std::string::npos && dots == std::string::npos) return qualified;
    if (cols == std::string::npos) return qualified.substr(dots + 1);
    if (dots == std::string::npos || cols > dots) return qualified.substr(cols + 2);
    return qualified.substr(dots + 1);
}

std::string Method::signature() const {
    std::ostringstream out;
    out << name << "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) out << ", ";
        out << params[i].first;
        if (!params[i].second.empty()) {
            out << " " << params[i].second;
        }
    }
    out << ")";
    if (is_const) out << " const";
    return out.str();
}

const Class* ProjectModel::find_class(const std::string& qualified) const {
    for (const auto& item : classes) {
        if (item.qualified_name == qualified || item.name == qualified) {
            return &item;
        }
    }
    return nullptr;
}

std::string format_model(const ProjectModel& model) {
    std::ostringstream out;
    out << "ROOT " << model.root << "\n";
    auto classes = model.classes;
    std::sort(classes.begin(), classes.end(), [](const Class& a, const Class& b) {
        return a.qualified_name < b.qualified_name;
    });
    for (const auto& item : classes) {
        out << "CLASS " << item.qualified_name << " " << item.file << ":" << item.line << "\n";
        for (const auto& base : item.bases) {
            out << "  BASE " << base.access << " " << base.name << "\n";
        }
        for (const auto& field : item.fields) {
            out << "  FIELD " << field.access << " " << field.name << " " << field.type_spelling;
            if (!field.target.empty()) {
                out << " " << relation_name(field.relation) << " -> " << field.target;
            }
            out << "\n";
        }
        for (const auto& method : item.methods) {
            out << "  METHOD " << method.access << " " << method.signature();
            if (!method.return_type.empty()) {
                out << " -> " << method.return_type;
            }
            out << "\n";
        }
    }
    auto relations = model.relations;
    std::sort(relations.begin(), relations.end(), [](const Relation& a, const Relation& b) {
        if (a.from != b.from) return a.from < b.from;
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        if (a.to != b.to) return a.to < b.to;
        return a.label < b.label;
    });
    for (const auto& relation : relations) {
        out << "RELATION " << relation.from << " " << relation_name(relation.kind) << " " << relation.to;
        if (!relation.label.empty()) {
            out << " " << relation.label;
        }
        out << "\n";
    }
    auto flows = model.flows;
    std::sort(flows.begin(), flows.end(), [](const FunctionFlow& a, const FunctionFlow& b) {
        return a.qualified_name < b.qualified_name;
    });
    for (const auto& flow : flows) {
        out << "FLOW " << flow.qualified_name << " " << flow.file << ":" << flow.line << "\n";
        for (const auto& step : flow.steps) {
            out << "  STEP " << step.depth << " " << step.kind << " " << step.text << "\n";
        }
    }
    for (const auto& call : model.calls) {
        out << "CALL " << call.caller << " -> " << call.callee;
        if (!call.args.empty()) {
            out << " (" << call.args << ")";
        }
        out << "\n";
    }
    for (const auto& diagnostic : model.diagnostics) {
        out << "DIAG " << diagnostic << "\n";
    }
    return out.str();
}

}  // namespace rewire
