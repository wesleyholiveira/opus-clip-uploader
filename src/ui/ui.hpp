#pragma once

void open_settings(void *private_data);
void open_confirm_dialog(void *private_data);

class QWidget;

void ensure_google_access_token(QWidget *parent);