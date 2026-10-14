idf_component_register(SRCS "src/argus_face_attendance.cc"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash
                                     spiffs
                                     esp_http_client
                                     espressif__esp_video
                                     espressif__human_face_detect
                                     espressif__human_face_recognition)
