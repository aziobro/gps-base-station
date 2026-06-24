ARG IDF_IMAGE=espressif/idf:v6.0.1
FROM ${IDF_IMAGE}

SHELL ["/bin/bash", "-lc"]

ENV IDF_TARGET=esp32p4
WORKDIR /work

COPY docker/entrypoint.sh /usr/local/bin/gbs-idf-entrypoint
RUN sed -i 's/\r$//' /usr/local/bin/gbs-idf-entrypoint \
    && chmod +x /usr/local/bin/gbs-idf-entrypoint

ENTRYPOINT ["/usr/local/bin/gbs-idf-entrypoint"]
CMD ["idf.py", "-B", "build-docker", "build"]
