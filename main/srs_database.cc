#include "srs_database.h"
#include <esp_log.h>
#include <fstream>
#include <algorithm>
#include <ctime>
#include <esp_spiffs.h>
#include <sys/stat.h>
#include "cJSON.h" // For JSON handling
#include <esp_lvgl_port.h>

#define TAG "SrsDatabase"

void SrsDatabase::Initialize(const char* mount_point) {
    mount_point_ = mount_point;
    bin_path_ = mount_point_ + "/srs_records.bin";
    json_path_ = mount_point_ + "/srs_daily.json";
    
    // Register SPIFFS if it hasn't been registered by something else
    // We assume the caller might pass a specific partition label
    // If the mount point is e.g. "/spiffs" we might try mounting "spiffs"
    std::string partition_label = mount_point;
    if (partition_label.length() > 0 && partition_label[0] == '/') {
        partition_label = partition_label.substr(1);
    }
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_point,
        .partition_label = partition_label.c_str(),
        .max_files = 5,
        .format_if_mount_failed = true, // We should format if it fails so we have a working DB
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized for SRS Database at %s", mount_point);
    }

    LoadDatabase();
}

void SrsDatabase::LoadDatabase() {
    records_.clear();
    std::ifstream file(bin_path_, std::ios::binary);
    if (!file) {
        ESP_LOGI(TAG, "Database file not found at %s. Starting fresh.", bin_path_.c_str());
        return;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size > 0 && file_size % sizeof(SrsRecord) == 0) {
        size_t num_records = file_size / sizeof(SrsRecord);
        records_.resize(num_records);
        if (file.read(reinterpret_cast<char*>(records_.data()), file_size)) {
            ESP_LOGI(TAG, "Loaded %zu records from database.", num_records);
        } else {
            ESP_LOGE(TAG, "Failed to read database file.");
            records_.clear();
        }
    } else {
        ESP_LOGW(TAG, "Database file size %zu is invalid. Starting fresh.", file_size);
    }
}

void SrsDatabase::SaveDatabase() {
    std::ofstream file(bin_path_, std::ios::binary | std::ios::trunc);
    if (!file) {
        ESP_LOGE(TAG, "Failed to open database file for writing at %s", bin_path_.c_str());
        return;
    }

    if (records_.empty()) return;

    if (file.write(reinterpret_cast<const char*>(records_.data()), records_.size() * sizeof(SrsRecord))) {
        ESP_LOGI(TAG, "Saved %zu records to database.", records_.size());
    } else {
        ESP_LOGE(TAG, "Failed to write database file.");
    }
}

SrsRecord SrsDatabase::GetCard(uint32_t card_id) {
    auto it = std::find_if(records_.begin(), records_.end(), [card_id](const SrsRecord& r) {
        return r.card_id == card_id;
    });

    if (it != records_.end()) {
        return *it;
    }

    SrsRecord default_record = {0};
    default_record.card_id = card_id;
    default_record.due_timestamp = 0;
    default_record.ease_factor = 2.5f;
    default_record.interval_days = 0;
    default_record.repetition_n = 0;
    default_record.total_reviews = 0;
    default_record.total_correct = 0;
    return default_record;
}

void SrsDatabase::UpdateCard(const SrsRecord& rec) {
    auto it = std::find_if(records_.begin(), records_.end(), [&rec](const SrsRecord& r) {
        return r.card_id == rec.card_id;
    });

    if (it != records_.end()) {
        *it = rec;
    } else {
        records_.push_back(rec);
    }
    
    SaveDatabase();
}

std::vector<uint32_t> SrsDatabase::GetDueCardIds() {
    std::vector<uint32_t> due_cards;
    time_t now = time(nullptr);

    for (const auto& rec : records_) {
        if (rec.due_timestamp <= (uint32_t)now) {
            due_cards.push_back(rec.card_id);
        }
    }
    return due_cards;
}

int SrsDatabase::GetDueCount() {
    return GetDueCardIds().size();
}

SrsSessionStats SrsDatabase::GetStats() {
    SrsSessionStats stats;
    time_t now = time(nullptr);

    for (const auto& rec : records_) {
        stats.total_reviews += rec.total_reviews;
        stats.total_correct += rec.total_correct;
        
        if (rec.due_timestamp <= (uint32_t)now) {
            stats.cards_due_today++;
        }
        
        if (rec.interval_days >= 21) {
            stats.cards_mastered++;
        } else if (rec.interval_days < 7) {
            stats.cards_learning++;
        }
    }
    return stats;
}

void SrsDatabase::SaveSession(uint32_t card_id, bool correct) {
    SrsRecord rec = GetCard(card_id);
    rec.total_reviews++;
    if (correct) {
        rec.total_correct++;
    }
    
    UpdateCard(rec);
    
    // Update daily counts JSON
    std::vector<int> counts = GetDailyReviewCounts(30);
    if (!counts.empty()) {
        counts[0]++; // Increment today's count
    } else {
        counts.push_back(1);
    }
    SaveDailyCounts(counts);
}

std::vector<int> SrsDatabase::GetDailyReviewCounts(int days) {
    std::vector<int> counts(days, 0);
    std::ifstream file(json_path_);
    if (!file) return counts;

    std::string json_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (root) {
        cJSON* array = cJSON_GetObjectItem(root, "daily_counts");
        if (array && cJSON_IsArray(array)) {
            int array_size = cJSON_GetArraySize(array);
            for (int i = 0; i < std::min(days, array_size); i++) {
                cJSON* item = cJSON_GetArrayItem(array, i);
                if (cJSON_IsNumber(item)) {
                    counts[i] = item->valueint;
                }
            }
        }
        cJSON_Delete(root);
    }
    return counts;
}

void SrsDatabase::SaveDailyCounts(const std::vector<int>& counts) {
    cJSON* root = cJSON_CreateObject();
    cJSON* array = cJSON_CreateArray();
    for (int count : counts) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(count));
    }
    cJSON_AddItemToObject(root, "daily_counts", array);

    char* str = cJSON_PrintUnformatted(root);
    if (str) {
        std::ofstream file(json_path_, std::ios::trunc);
        if (file) {
            file << str;
        }
        cJSON_free(str);
    }
    cJSON_Delete(root);
}

void SrsDatabase::ShowAnalyticsUI(lv_obj_t* parent) {
    if (!lvgl_port_lock(0)) return;

    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    
    lv_obj_t* label = lv_label_create(container);
    lv_label_set_text(label, "SRS Analytics");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);
    
#if LV_USE_CHART
    lv_obj_t* chart = lv_chart_create(container);
    lv_obj_set_size(chart, 200, 150);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    
    lv_chart_series_t* ser = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    std::vector<int> daily_counts = GetDailyReviewCounts(7);
    
    for (int i = 0; i < 7; i++) {
        int val = (i < (int)daily_counts.size()) ? daily_counts[i] : 0;
        lv_chart_set_next_value(chart, ser, val);
    }
#else
    lv_obj_t* no_chart_label = lv_label_create(container);
    lv_label_set_text(no_chart_label, "Chart disabled in config");
    lv_obj_align(no_chart_label, LV_ALIGN_CENTER, 0, 0);
#endif

    lvgl_port_unlock();
}
