#pragma once

#include <functional>
#include <memory>

#include "ui/elements.h"
#include "ui/generated_values.h"

namespace wb::ui {

// The callback is only a request to the host; durable binding, authorization and idempotency remain host responsibilities.
class GeneratedActionView : public Box {
public:
    // The host calls completion on the UI thread after durable acceptance or a retryable failure.
    using Submit = std::function<void(std::string, std::vector<GeneratedFieldValue>, std::function<void(bool)>)>;
    GeneratedActionView(Ui& ui, json action, json surface, std::shared_ptr<json> draft, Submit submit,
                        std::function<json()> sections = {});

private:
    void review();
    void close_review();
    void show_status(const char* key, bool success = false);
    Ui& ui_;
    json action_, surface_;
    std::shared_ptr<json> draft_;
    Submit submit_;
    std::function<json()> sections_;
    Node* panel_ = nullptr;
    Label* status_ = nullptr;
    Button* review_button_ = nullptr;
    Button* confirm_ = nullptr;
    Button* cancel_ = nullptr;
    json reviewed_;
    bool accepted_ = false;
    bool pending_ = false;
    uint64_t request_ = 0;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace wb::ui
