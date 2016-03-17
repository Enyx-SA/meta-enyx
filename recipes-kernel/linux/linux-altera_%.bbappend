do_configure_append() {
    sed -i "s|^.*CONFIG_DYNAMIC_DEBUG[ =].*$|CONFIG_DYNAMIC_DEBUG=y|g" .config
}
