#include "edit.hpp"

#include <sstream>

namespace rewire {
namespace {

const char* action_name(EditAction action) {
    switch (action) {
        case EditAction::AddClass: return "add_class";
        case EditAction::RenameClass: return "rename_class";
        case EditAction::AddField: return "add_field";
        case EditAction::AddMethod: return "add_method";
        case EditAction::AddBase: return "add_base";
        case EditAction::RemoveMember: return "remove_member";
    }
    return "edit";
}

std::string relationship_guidance(const std::string& language, const std::string& relationship) {
    if (language == "Java") {
        if (relationship == "composition") return "Store it as a private final field and construct it in this class.";
        if (relationship == "aggregation") return "Store it in a List, Set, Map, or array. The collection holds references.";
        if (relationship == "association") return "Store it as a non-final field. Do not own its lifetime.";
        if (relationship == "dependency") return "Do not store it. Use it as a parameter, return type, local, or call.";
        if (relationship == "inheritance") return "Add it with extends, or implements when it is an interface.";
    } else if (language == "Python") {
        if (relationship == "composition") return "Assign self.name = Type(...) inside __init__ so this class constructs it.";
        if (relationship == "aggregation") return "Store it as a list, set, or dict annotation, for example self.items: list[Part] = [].";
        if (relationship == "association") return "Assign a self field from a parameter or an existing object. Do not construct it here.";
        if (relationship == "dependency") return "Do not store it on self. Use it as a parameter, annotation, or call.";
        if (relationship == "inheritance") return "Put the base in the class header: class Child(Base).";
    } else if (language == "TypeScript") {
        if (relationship == "composition") return "Store it as a readonly field, or construct it with new in this class.";
        if (relationship == "aggregation") return "Store it as T[] or Array<T>, Set<T>, or Map<K, V>.";
        if (relationship == "association") return "Store it as a field of that class. Do not mark it readonly unless this class owns it.";
        if (relationship == "dependency") return "Do not store it. Use it as a parameter, return type, local, or call.";
        if (relationship == "inheritance") return "Add extends for a class, or implements for an interface.";
    } else if (language == "Rust") {
        if (relationship == "composition") return "Store it as an owned field: the type itself, Box<T>, or Vec<T>.";
        if (relationship == "aggregation") return "Store it as Rc<T> or Arc<T>.";
        if (relationship == "association") return "Store a non-owning link: a reference, or Weak<T>.";
        if (relationship == "dependency") return "Do not store it. Take it as a parameter or use it as a local.";
        if (relationship == "inheritance") return "Rust has no class inheritance. impl Trait for Type, or add the trait as a supertrait.";
    } else if (language == "C") {
        if (relationship == "composition") return "Embed the struct by value, or store an array of that struct.";
        if (relationship == "aggregation") return "C has no shared-ownership type. Use an association pointer and document the shared lifetime.";
        if (relationship == "association") return "Store a pointer to the struct. Do not free it if another owner does.";
        if (relationship == "dependency") return "Do not store it. Use it as a parameter or a local.";
        if (relationship == "inheritance") return "C has no base classes. Embed the other struct as a member, or keep a pointer to it.";
    } else {
        if (relationship == "composition") return "Store the part so this class owns its lifetime. Use a value member, std::unique_ptr, or a container of values.";
        if (relationship == "aggregation") return "Store the part as std::shared_ptr. Lifetime is shared, not exclusive.";
        if (relationship == "association") return "Store a non-owning link: a raw pointer or a reference. Do not delete it in this class.";
        if (relationship == "dependency") return "Do not store the type. Use it as a parameter, return type, or local.";
        if (relationship == "inheritance") return "Add it as a base class.";
    }
    return "Match the surrounding style of this " + language + " project.";
}

std::string class_snapshot(const Class& item) {
    std::ostringstream out;
    out << item.qualified_name << " in " << item.file << ":" << item.line << "\n";
    for (const auto& base : item.bases) {
        out << "  base " << base.access << " " << base.name << "\n";
    }
    for (const auto& field : item.fields) {
        out << "  field " << field.access << " " << field.type_spelling << " " << field.name;
        if (!field.target.empty()) out << " [" << relation_name(field.relation) << " -> " << field.target << "]";
        out << "\n";
    }
    for (const auto& method : item.methods) {
        out << "  method " << method.access << " " << method.signature();
        if (!method.return_type.empty()) out << " -> " << method.return_type;
        out << "\n";
    }
    return out.str();
}

}  // namespace

PreparedEdit prepare_edit(const ProjectModel& model, const EditRequest& request) {
    PreparedEdit prepared;
    const bool needs_class = request.action != EditAction::AddClass;
    const Class* target = nullptr;
    if (needs_class) {
        if (request.class_name.empty()) {
            prepared.error = "Choose a class first.";
            return prepared;
        }
        target = model.find_class(request.class_name);
        if (!target) {
            prepared.error = "No class named " + request.class_name + " is in the diagram.";
            return prepared;
        }
    }
    if (request.action == EditAction::AddClass && request.primary.empty()) {
        prepared.error = "Enter the new class name.";
        return prepared;
    }
    if ((request.action == EditAction::AddField || request.action == EditAction::RemoveMember ||
         request.action == EditAction::RenameClass || request.action == EditAction::AddBase ||
         request.action == EditAction::AddMethod) &&
        request.primary.empty()) {
        prepared.error = "Fill in the edit before sending it.";
        return prepared;
    }
    if (request.action == EditAction::AddField && request.secondary.empty()) {
        prepared.error = "Enter the field type.";
        return prepared;
    }

    std::ostringstream summary;
    switch (request.action) {
        case EditAction::AddClass:
            summary << "Add class " << request.primary;
            if (!request.secondary.empty()) summary << " : " << request.secondary;
            break;
        case EditAction::RenameClass:
            summary << "Rename " << target->qualified_name << " to " << request.primary;
            break;
        case EditAction::AddField:
            summary << "Add " << request.relationship << " field " << request.primary << " : " << request.secondary
                    << " to " << target->qualified_name;
            break;
        case EditAction::AddMethod:
            summary << "Add " << request.access << " method " << request.primary << " to " << target->qualified_name;
            break;
        case EditAction::AddBase:
            summary << "Make " << target->qualified_name << " inherit " << request.primary;
            break;
        case EditAction::RemoveMember:
            summary << "Remove " << request.primary << " from " << target->qualified_name;
            break;
    }
    prepared.summary = summary.str();

    std::ostringstream prompt;
    const std::string file_for_language =
        target ? target->file : (model.classes.empty() ? std::string() : model.classes.front().file);
    const std::string language = project_language(model, file_for_language);
    prompt << "Rewire diagram edit. The user changed the UML diagram in the Rewire harness and wants that change applied to the source.\n\n";
    prompt << "Language: " << language << "\n";
    prompt << "Project root: " << model.root << "\n";
    prompt << "Apply only this change. Read the existing files first. Match the surrounding style. Do not refactor unrelated code.\n\n";
    prompt << "REWIRE-EDIT\n";
    prompt << "action: " << action_name(request.action) << "\n";
    if (target) {
        prompt << "class: " << target->qualified_name << "\n";
        prompt << "file: " << target->file << "\n";
        prompt << "line: " << target->line << "\n";
    }
    prompt << "primary: " << request.primary << "\n";
    if (!request.secondary.empty()) prompt << "secondary: " << request.secondary << "\n";
    prompt << "access: " << request.access << "\n";
    prompt << "relationship: " << request.relationship << "\n";
    prompt << "END\n\n";
    prompt << relationship_guidance(language, request.relationship) << "\n\n";
    if (request.action == EditAction::AddClass) {
        prompt << "Create the class in this project. If the tree has include/ and src/, put the header and the implementation there. "
                  "If a base was given, inherit from it.\n";
    } else if (request.action == EditAction::AddField) {
        prompt << "Add the field to the existing class and initialize it in the constructors that need it.\n";
    } else if (request.action == EditAction::AddMethod) {
        prompt << "The primary value is the method signature, for example 'bool cancel(int order_id)'. "
                  "Declare it on the class and define it, with a straightforward body when the behavior is obvious.\n";
    } else if (request.action == EditAction::RenameClass) {
        prompt << "Rename the class, its constructors, and the references inside this project. Leave unrelated names alone.\n";
    } else if (request.action == EditAction::AddBase) {
        prompt << "Add the base specifier and include the header that declares the base.\n";
    } else if (request.action == EditAction::RemoveMember) {
        prompt << "Remove that field or method and the code that exists only to support it. Keep the class compiling.\n";
    }
    if (target) {
        prompt << "\nCurrent diagram entry:\n" << class_snapshot(*target);
    }
    prompt << "\nWhen the edit is done, reply with one line listing the files you changed.\n";
    prepared.prompt = prompt.str();
    return prepared;
}

std::string source_language(const std::string& file) {
    auto dot = file.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : file.substr(dot);
    if (ext == ".java") return "Java";
    if (ext == ".py") return "Python";
    if (ext == ".ts" || ext == ".tsx") return "TypeScript";
    if (ext == ".rs") return "Rust";
    if (ext == ".c") return "C";
    return "C++";
}

std::string project_language(const ProjectModel& model, const std::string& file) {
    std::string language = source_language(file);
    if (file.size() >= 2 && file.ends_with(".h")) {
        bool saw_c = false;
        bool saw_cxx = false;
        for (const auto& item : model.classes) {
            if (item.file.size() >= 2 && item.file.ends_with(".c")) saw_c = true;
            if (item.file.ends_with(".cpp") || item.file.ends_with(".cc") || item.file.ends_with(".cxx") ||
                item.file.ends_with(".hpp") || item.file.ends_with(".hh") || item.file.ends_with(".hxx")) {
                saw_cxx = true;
            }
        }
        if (saw_c && !saw_cxx) language = "C";
    }
    return language;
}

}  // namespace rewire
